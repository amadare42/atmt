// callers_of.java - who calls an internal function (by address)?
//
//   run_script.cmd callers_of.java 0x00484da0 [0x...]
//
// find_calls.java answers "which game code calls this *imported* symbol" (it only looks at
// external symbols). For an internal function - e.g. the save requester we are about to call
// from the -al mod - the question is which functions reference its entry point, so that the
// menu's own action can be identified and the call replayed without the menu.
//
// Thunks/jump stubs are followed (a game this size reaches half its code through them), and the
// output is indented per hop so the chain reads top-down.
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class callers_of extends GhidraScript {

    private boolean isThunk(Function func) {
        int instructions = 0;
        boolean hasJump = false;
        for (Instruction ins : currentProgram.getListing().getInstructions(func.getBody(), true)) {
            instructions++;
            if (instructions > 4) {
                return false;
            }
            if (ins.getMnemonicString().toUpperCase().startsWith("JMP")) {
                hasJump = true;
            }
        }
        return hasJump;
    }

    private void dump(Function func, int depth) {
        if (depth > 4) {
            return;
        }
        StringBuilder pad = new StringBuilder();
        for (int i = 0; i < depth; ++i) {
            pad.append("    ");
        }
        int shown = 0;
        for (Reference ref : getReferencesTo(func.getEntryPoint())) {
            Address from = ref.getFromAddress();
            Function cf = getFunctionContaining(from);
            if (cf == null) {
                println(pad + "  <- (data/no function) at " + from);
                continue;
            }
            if (cf.getEntryPoint().equals(func.getEntryPoint())) {
                continue;
            }
            shown++;
            println(pad + "  <- " + cf.getName(true) + " @0x" + cf.getEntryPoint() + "   from " + from
                        + (isThunk(cf) ? "   [thunk]" : ""));
            if (isThunk(cf)) {
                dump(cf, depth + 1);
            }
        }
        if (shown == 0) {
            println(pad + "  (no callers - entry point reached another way, e.g. a table)");
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: callers_of.java <0xADDRESS> [more ...] [--depth N]");
            return;
        }
        int maxDepth = 1;
        for (int i = 0; i < args.length; ++i) {
            if (args[i].equals("--depth") && i + 1 < args.length) {
                maxDepth = Integer.parseInt(args[i + 1]);
            }
        }
        for (String arg : args) {
            if (arg.startsWith("--")) {
                continue;
            }
            Address addr = toAddr(arg);
            Function f = getFunctionContaining(addr);
            if (f == null) {
                println("  no function at " + addr);
                continue;
            }
            println("");
            println("  " + f.getName(true) + " @0x" + f.getEntryPoint()
                        + (f.getEntryPoint().equals(addr) ? "" : "   (address is inside this function)"));
            dump(f, 1);
        }
    }
}
