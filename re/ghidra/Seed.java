// Seeds code entry points for a raw RP2040 flash image: boot2 and the Cortex-M0+ vector table.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.*;
import ghidra.program.model.symbol.*;

public class Seed extends GhidraScript {
    @Override
    public void run() throws Exception {
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        Register tmode = currentProgram.getLanguage().getRegister("TMode");
        long vt = 0x10000100L;
        // boot2 stage: thumb code at flash base
        seed(sp.getAddress(0x10000000L | 1), tmode, "boot2");
        for (int i = 1; i < 48; i++) {
            long v = currentProgram.getMemory().getInt(sp.getAddress(vt + i * 4)) & 0xffffffffL;
            if (v >= 0x10000100L && v < 0x10041000L && (v & 1) == 1) seed(sp.getAddress(v), tmode, "vec" + i);
        }
    }
    private void seed(Address a, Register tmode, String name) throws Exception {
        Address even = a.subtract(a.getOffset() & 1);
        currentProgram.getProgramContext().setValue(tmode, even, even, java.math.BigInteger.ONE);
        disassemble(even);
        createFunction(even, name);
    }
}
