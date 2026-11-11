#pragma once

#include <stdint.h>

#include "../third_party/cueparser/cueparser.h"
#include "../third_party/cueparser/disc.h"
#include "../third_party/cueparser/scheduler.h"
#include "../third_party/posix_file.h"
#include "ff.h"
#include "subq.h"

namespace picostation {
class DiscImage {
  public:
    DiscImage() {};
    ~DiscImage() {};

    enum DataLocation {
        RAM,
        SDCard
    };

    void buildSector(const int sector, uint32_t *buffer, uint16_t *userData, const uint16_t *scramling, bool pregap = false);
    FRESULT load(const TCHAR *targetCue);
    void unload();
    SubQ::Data generateSubQ(const int sector);
    bool hasData() { return m_hasData; };
    // V3: mounts the default disc through mh::hooks::mount_default_disc; false when nothing could be set up.
    bool makeDummyCue();
    // Upstream makeDummyCue (single 74 minute data track), kept as the fallback of the weak mount hook.
    void buildPlaceholderCue();
    void readSector(void *buffer, const int sector, DataLocation location, const uint16_t *scramling);
    void set_skip_bootsector(bool skip) { skip_bootsector =  skip; }
	void set_skip_edc(bool skip) { skip_edc =  skip; }
    // V3 added a byte after the two skip flags (DiscImage +0x147A): non zero routes readSector to the card layer.
    void set_card_mode(bool on) { m_cardMode = on; }
    bool card_mode() const { return m_cardMode; }

  private:
    CueDisc m_cueDisc;
    bool m_hasData = false;
    int m_currentLogicalTrack = 0;
    bool skip_bootsector = false;
    bool skip_edc = false;
    bool m_cardMode = false;
};

extern DiscImage g_discImage;

// The Multi House state block and the card layer address the disc by raw offsets (CardDisc in mh/card.h).
static_assert(sizeof(DiscImage) == 0x147C, "V3 DiscImage layout");
}  // namespace picostation
