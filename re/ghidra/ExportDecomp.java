// Exports every function (address, size, decompiled C) and all defined strings with xrefs.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "/tmp/decomp";
        new File(out).mkdirs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        PrintWriter all = new PrintWriter(new FileWriter(out + "/all.c"));
        PrintWriter idx = new PrintWriter(new FileWriter(out + "/functions.tsv"));
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        int n = 0;
        while (it.hasNext()) {
            Function f = it.next();
            long size = f.getBody().getNumAddresses();
            idx.println(f.getEntryPoint() + "\t" + size + "\t" + f.getName());
            DecompileResults r = di.decompileFunction(f, 60, monitor);
            all.println("// ==== " + f.getEntryPoint() + " " + f.getName() + " size=" + size);
            if (r.decompileCompleted()) all.println(r.getDecompiledFunction().getC());
            else all.println("// decompile failed: " + r.getErrorMessage());
            n++;
        }
        all.close(); idx.close();
        PrintWriter st = new PrintWriter(new FileWriter(out + "/strings.tsv"));
        DataIterator di2 = currentProgram.getListing().getDefinedData(true);
        while (di2.hasNext()) {
            Data d = di2.next();
            if (d.hasStringValue()) {
                StringBuilder refs = new StringBuilder();
                for (Reference ref : getReferencesTo(d.getAddress())) refs.append(ref.getFromAddress()).append(' ');
                st.println(d.getAddress() + "\t" + refs + "\t" + d.getDefaultValueRepresentation());
            }
        }
        st.close();
        println("exported " + n + " functions");
    }
}
