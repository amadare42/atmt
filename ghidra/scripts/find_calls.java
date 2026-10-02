// find_calls.java - which game functions call an imported function (e.g. fread)?
//
//   run_script.cmd find_calls.java fread _read
//
// The file probe showed the save data being read through the CRT (msvcr100's fread/_read), so the
// interesting game code is whatever called *that*. This lists the functions that reference an
// imported symbol whose name contains one of the arguments, with thunk expansion - the same
// navigation that dump_at.java does for a single address, but driven by an import name.
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class find_calls extends GhidraScript {

    private boolean isThunk(Function func) {
        int instructions = 0;
        boolean hasJump = false;
        for (Instruction ins : currentProgram.getListing().getInstructions(func.getBody(), true)) {
            instructions++;
            if (instructions > 4) {
                return false;
            }
            String text = ins.getMnemonicString().toUpperCase();
            if (text.startsWith("JMP")) {
                hasJump = true;
            }
        }
        return hasJump;
    }

    private void dumpCallersOfThunk(Function thunk, int depth) {
        if (depth > 3) {
            return;
        }
        for (Reference ref : getReferencesTo(thunk.getEntryPoint())) {
            Address from = ref.getFromAddress();
            Function cf = getFunctionContaining(from);
            if (cf == null) {
                println("        via thunk -> (no function) at " + from);
                continue;
            }
            println("        via thunk -> " + cf.getName(true) + " @" + cf.getEntryPoint()
                    + "   at " + from);
            if (isThunk(cf)) {
                dumpCallersOfThunk(cf, depth + 1);
            }
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: find_calls.java <symbol substring> [more ...]");
            return;
        }

        SymbolIterator symbols = currentProgram.getSymbolTable().getAllSymbols(true);
        int matched = 0;
        while (symbols.hasNext()) {
            Symbol sym = symbols.next();
            String name = sym.getName();
            String lower = name.toLowerCase();
            boolean wanted = false;
            for (String arg : args) {
                if (lower.contains(arg.toLowerCase())) {
                    wanted = true;
                }
            }
            if (!wanted || !sym.isExternal()) {
                continue;
            }
            matched++;
            println("");
            println("  symbol " + name + "  @ " + sym.getAddress()
                    + (sym.getParentNamespace() != null ? "  (" + sym.getParentNamespace().getName() + ")" : ""));
            int callers = 0;
            for (Reference ref : getReferencesTo(sym.getAddress())) {
                Address from = ref.getFromAddress();
                Function cf = getFunctionContaining(from);
                if (cf == null) {
                    continue;
                }
                callers++;
                println("      called by " + cf.getName(true) + " @" + cf.getEntryPoint() + "   at " + from);
                if (isThunk(cf)) {
                    dumpCallersOfThunk(cf, 1);
                }
            }
            if (callers == 0) {
                println("      (no callers found)");
            }
        }
        println("");
        println("  external symbols matching: " + matched);
    }
}
