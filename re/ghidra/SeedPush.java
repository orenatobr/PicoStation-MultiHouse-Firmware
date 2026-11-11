// Creates functions at Thumb "push {..., lr}" prologues that analysis left outside any function.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;

public class SeedPush extends GhidraScript {
    @Override
    public void run() throws Exception {
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        int added = 0;
        long[][] ranges = {{0x10000100L, 0x10030190L}, {0x200000c0L, 0x20008f68L}};
        for (long[] r : ranges) {
            for (long a = r[0]; a < r[1]; a += 2) {
                Address ad = sp.getAddress(a);
                int hw = currentProgram.getMemory().getShort(ad) & 0xffff;
                if ((hw & 0xff00) != 0xb500) continue;      // push {r0-r7, lr}
                if (getFunctionContaining(ad) != null) continue;
                if (getInstructionContaining(ad) != null && getInstructionAt(ad) == null) continue;
                if (disassemble(ad)) {
                    if (createFunction(ad, null) != null) added++;
                }
            }
        }
        println("seeded " + added + " functions");
    }
}
