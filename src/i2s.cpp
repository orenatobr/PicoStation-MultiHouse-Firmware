#include "i2s.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <array>

#include "cmd.h"
#include "disc_image.h"
#include "drive_mechanics.h"
#include "ff.h"
#include "global.h"
#include "hardware/pio.h"
#include "logging.h"
#include "main.pio.h"
#include "modchip.h"
#include "pico/stdlib.h"
#include "picostation.h"
#include "pseudo_atomics.h"
#include "subq.h"
#include "values.h"
#include "mh/hooks.h"

#if DEBUG_I2S
#define DEBUG_PRINT printf
#else
#define DEBUG_PRINT(...) while (0)
#endif

// V3 still defines and initialises the three file browser globals of upstream, nothing reads them any more.
pseudoatomic<int> needFileCheckAction;
pseudoatomic<int> listReadyState;
pseudoatomic<int> g_entryOffset;

// V3: the RAM menu location is gone, every sector comes from the mounted image (cue file or card).
picostation::DiscImage::DataLocation s_dataLocation = picostation::DiscImage::DataLocation::SDCard;

static uint16_t *__time_critical_func(generateScramblingLUT)()
{
    static uint16_t ScramblingLUT[1176] = {0};
    int shift = 1;
	
	for (int i = 0; i < 6; i++)
	{
		ScramblingLUT[i] = 0;
	}
	
    for (size_t i = 6; i < 1176; i++)
    {
        uint8_t upper = shift & 0xFF;
        for (size_t j = 0; j < 8; j++)
        {
            unsigned bit = ((shift & 1) ^ ((shift & 2) >> 1)) << 15;
            shift = (bit | shift) >> 1;
        }

        uint8_t lower = shift & 0xFF;

        ScramblingLUT[i] = (lower << 8) | upper;

        for (size_t j = 0; j < 8; j++)
        {
            unsigned bit = ((shift & 1) ^ ((shift & 2) >> 1)) << 15;
            shift = (bit | shift) >> 1;
        }
    }

    return ScramblingLUT;
}

int __time_critical_func(picostation::I2S::initDMA)(const volatile void *read_addr, unsigned int transfer_count)
{
    int channel = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(channel);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    const unsigned int i2sDREQ = PIOInstance::I2S_DATA == pio0 ? DREQ_PIO0_TX0 : DREQ_PIO1_TX0;
    channel_config_set_dreq(&c, i2sDREQ);
    dma_channel_configure(channel, &c, &PIOInstance::I2S_DATA->txf[SM::I2S_DATA], read_addr, transfer_count, false);

    return channel;
}

[[noreturn]] void __time_critical_func(picostation::I2S::start)(MechCommand &mechCommand)
{
    picostation::ModChip modChip;
    static uint32_t pioSamples[CACHED_SECS][1176];
    static uint16_t *cdScramblingLUT = generateScramblingLUT();

    static uint8_t bufferForDMA = 1;
    static uint8_t bufferForSDRead = 0;
    static int currentSector = -1;
    lastSector = -1;
    m_sectorSending = -1;
    static uint32_t loadedImageIndex = 0;
    static uint16_t img_count;

    needFileCheckAction = 0;
    listReadyState = 1;
    g_entryOffset = 0;
    
    menu_active = false;
    s_doorPending = false;
    
    reinitI2S();
	
    dmaChannel = initDMA(pioSamples[0], 1176);

    g_coreReady[1] = true;          // Core 1 is ready
    while (!g_coreReady[0].Load())  // Wait for Core 0 to be ready
    {
        tight_loop_contents();
    }

    modChip.init();

    // V3 FUN_10030000: mounts the card (or its fallback menu); the single image counts as disc 0 of 1.
    if (g_discImage.makeDummyCue())
    {
        loadedImageIndex = 0;
        img_count = 1;
        reinitI2S();
        g_driveMechanics.resetDrive();
        g_discImage.set_skip_bootsector(true);
        mechCommand.resetBootSectorPattern();
    }

    while (true)
    {
        // Sector could change during the loop, so we need to keep track of it
        currentSector = g_driveMechanics.getSector();
        
        modChip.sendLicenseString(currentSector, mechCommand);

        // V3 stubs (RAM 0x20033100 / 0x2003311c): test and clear the door flag, rotate the legacy disc when it
        // was set, then let the card layer finish a pending command.
        const bool doorPending = s_doorPending.Load();
        s_doorPending = false;
        if (doorPending)
        {
            mh::hooks::on_door();
        }
        mh::hooks::poll();
		
        // Data sent via DMA, load the next sector
        if (currentSector != lastSector && currentSector >= 4503 && currentSector < c_sectorMax)
        {
			if (!menu_active.Load())
			{
				for (int i = 0; i < CACHED_SECS; i++)
				{
					if (loadedSector[i] == currentSector)
					{
						// already in cache
						bufferForDMA = i;
						lastSector = currentSector;
						goto continue_transfer;
					}
				}
			}
			
			while (bufferForSDRead == bufferForDMA)
			{
				++bufferForSDRead &= (CACHED_SECS-1);
			}
			
			// Load the next sector
			g_discImage.readSector(pioSamples[bufferForSDRead], currentSector - c_leadIn, s_dataLocation, cdScramblingLUT);
			
			loadedSector[bufferForSDRead] = currentSector;
			bufferForDMA = bufferForSDRead;
			lastSector = currentSector;
		}

continue_transfer:

        // Start the next transfer if the DMA channel is not busy
        if (!dma_channel_is_busy(dmaChannel) && i2s_state)
        {
			if (currentSector >= 4503 && currentSector < c_sectorMax)
			{
				m_sectorSending = loadedSector[bufferForDMA];
				m_lastSectorTime = time_us_64();

				dma_hw->ch[dmaChannel].read_addr = (uint32_t)pioSamples[bufferForDMA];

				// Sync with the I2S clock
				while (gpio_get(Pin::LRCK) == 1)
				{
					tight_loop_contents();
				}
				
				while (gpio_get(Pin::LRCK) == 0)
				{
					tight_loop_contents();
				}

				dma_channel_start(dmaChannel);
			}
			else if(picostation::g_subqDelay == false)
			{
				m_sectorSending = currentSector;
				m_lastSectorTime = time_us_64();
			}
        }
    }
    __builtin_unreachable();
}
