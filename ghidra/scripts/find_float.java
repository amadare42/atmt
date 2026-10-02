// find_float.java - locate float/double immediates equal to given values, and who references them.
//
//   run_script.cmd find_float.java 1280.0 720.0 1.77778
//
// UI reference-resolution / aspect-ratio constants are stored as float literals in .rdata, not as
// instruction immediates (x86 FPU/SSE loads them from memory) - this scans defined Data for a
// float or double matching (within a small tolerance) one of the requested values.
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class find_float extends GhidraScript {

    private static final double TOL = 0.001;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: find_float.java <value> [value ...]");
            return;
        }
        double[] targets = new double[args.length];
        for (int i = 0; i < args.length; i++) {
            targets[i] = Double.parseDouble(args[i]);
        }

        DataIterator it = currentProgram.getListing().getDefinedData(true);
        int[] shown = new int[args.length];
        while (it.hasNext()) {
            Data data = it.next();
            String dt = data.getDataType().getName().toLowerCase();
            if (!dt.contains("float") && !dt.contains("double")) {
                continue;
            }
            Object value = data.getValue();
            if (!(value instanceof Number)) {
                continue;
            }
            double dv = ((Number) value).doubleValue();
            for (int i = 0; i < targets.length; i++) {
                if (Math.abs(dv - targets[i]) > TOL) {
                    continue;
                }
                shown[i]++;
                println("");
                println("  [" + args[i] + "] " + data.getAddress() + "  (" + dt + ") = " + dv);
                int refs = 0;
                for (Reference ref : getReferencesTo(data.getAddress())) {
                    Address from = ref.getFromAddress();
                    Function f = getFunctionContaining(from);
                    println("      " + (f == null ? "(no function)" : f.getName(true) + " @" + f.getEntryPoint())
                            + "   use at " + from);
                    refs++;
                }
                if (refs == 0) {
                    println("      (no references)");
                }
            }
        }
        println("");
        for (int i = 0; i < args.length; i++) {
            println("  total \"" + args[i] + "\": " + shown[i]);
        }
    }
}
