// decompile.java - decompiled C for one or more functions (by address).
//
//   run_script.cmd decompile.java 0x00484da0 [0x...]
//
// Disassembly tells you what the instructions are; this tells you what the function *does*,
// which is what matters when the goal is to reuse a game function from a mod (e.g. the save
// subsystem's request entry point). Also prints the function's signature and its callers, so a
// single run answers "what is this, and who reaches it".
//
//@category atmt

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class decompile extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: decompile.java <0xADDRESS|symbol> [more ...]");
            return;
        }
        DecompInterface ifc = new DecompInterface();
        ifc.openProgram(currentProgram);
        for (String arg : args) {
            Function f = null;
            if (arg.startsWith("0x") || arg.startsWith("004") || arg.startsWith("005")) {
                Address addr = toAddr(arg);
                f = getFunctionContaining(addr);
                if (f == null) {
                    f = createFunction(addr, null);
                }
            } else {
                f = getGlobalFunctions(arg).isEmpty() ? null : getGlobalFunctions(arg).get(0);
            }
            if (f == null) {
                println("  (no function for " + arg + ")");
                continue;
            }
            println("");
            println("=== " + f.getName(true) + " @0x" + f.getEntryPoint() + " ===");
            println("  signature: " + f.getPrototypeString(true, false));
            int callers = 0;
            for (Reference ref : getReferencesTo(f.getEntryPoint())) {
                Function cf = getFunctionContaining(ref.getFromAddress());
                println("  referenced from " + ref.getFromAddress()
                            + (cf != null ? "  in " + cf.getName(true) + " @0x" + cf.getEntryPoint()
                                          : "  (data)"));
                if (cf != null) {
                    callers++;
                }
            }
            if (callers == 0) {
                println("  (no code callers: reached through a table or a callback)");
            }
            DecompileResults res = ifc.decompileFunction(f, 45, monitor);
            if (res != null && res.decompileCompleted()) {
                println("--- C ---");
                println(res.getDecompiledFunction().getC());
            } else {
                println("  (decompile failed: "
                            + (res == null ? "no result" : res.getErrorMessage()) + ")");
            }
        }
        ifc.dispose();
    }
}
