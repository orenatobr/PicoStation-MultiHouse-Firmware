// Maps the crt0 .data image (flash 0x100183cc, 0x8ea8 bytes) to RAM 0x200000c0 and the SIO/peripheral-free layout.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import java.io.ByteArrayInputStream;

public class MapRam extends GhidraScript {
    @Override
    public void run() throws Exception {
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        Memory mem = currentProgram.getMemory();
        byte[] b = new byte[0x8ea8];
        mem.getBytes(sp.getAddress(0x100183ccL), b);
        MemoryBlock blk = mem.createInitializedBlock(".data_ram", sp.getAddress(0x200000c0L), new ByteArrayInputStream(b), b.length, monitor, false);
        blk.setRead(true); blk.setWrite(true); blk.setExecute(true);
        MemoryBlock bss = mem.createUninitializedBlock(".bss_ram", sp.getAddress(0x20009900L), 0x2a000, false);
        bss.setRead(true); bss.setWrite(true);
        // seed: every Thumb vector target, plus disassemble the RAM image from its start
        disassemble(sp.getAddress(0x200000c0L));
    }
}
