// dump_at.java - read the code at an address our in-game probe reported.
//
// Ghidra 12 does not run Jython scripts any more, hence Java. Used headless:
//   ghidra\run_script.cmd dump_at.java 0x0046439c 0x00483e76
//
// For each address it prints the function it belongs to, that function's callers, references to
// the address itself (call sites), and a window of disassembly around it. That is what turns a
// "caller=0x0046439c" line from the probe into something readable.
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;

public class dump_at extends GhidraScript {

    private static final int WINDOW_BEFORE = 12;
    private static final int WINDOW_AFTER = 45;

    /** A thunk is a tiny function whose body is just a jump; the game is full of them (0x40xxxx). */
    private boolean isThunk(Function func) {
        int instructions = 0;
        boolean hasJump = false;
        for (Instruction ins : currentProgram.getListing().getInstructions(func.getBody(), true)) {
            instructions++;
            if (instructions > 4) {
                return false;
            }
            String text = ins.getMnemonicString().toUpperCase();
            if (text.startsWith("JMP") || text.startsWith("JMPF")) {
                hasJump = true;
            }
        }
        return hasJump;
    }

    /** Prints the callers of a thunk, so a call chain can be followed past the stub. */
    private void dumpThunkCallers(Function thunk, int depth) {
        if (depth > 2) {
            return;
        }
        for (Reference ref : getReferencesTo(thunk.getEntryPoint())) {
            Address from = ref.getFromAddress();
            Function cf = getFunctionContaining(from);
            if (cf == null) {
                println("      via thunk -> (no function) at " + from);
                continue;
            }
            println("      via thunk -> " + cf.getName(true) + " @" + cf.getEntryPoint()
                    + "   call at " + from);
            if (isThunk(cf)) {
                dumpThunkCallers(cf, depth + 1);
            }
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: dump_at.java 0xADDR [0xADDR ...]");
            return;
        }
        for (String arg : args) {
            long value = Long.parseLong(arg.replace("0x", "").replace("0X", ""), 16);
            Address addr = toAddr(value);
            println("");
            println("=== " + arg + " ===");

            Function func = getFunctionContaining(addr);
            if (func == null) {
                println("  in: (no function)");
            } else {
                println("  in: " + func.getName(true) + " @ " + func.getEntryPoint());
                println("  callers:");
                int callers = 0;
                for (Reference ref : getReferencesTo(func.getEntryPoint())) {
                    Address from = ref.getFromAddress();
                    Function cf = getFunctionContaining(from);
                    println("    " + (cf == null ? "(no function)" : cf.getName(true) + " @" + cf.getEntryPoint())
                            + "   call at " + from);
                    callers++;
                    if (cf != null && isThunk(cf)) {
                        dumpThunkCallers(cf, 1);
                    }
                }
                if (callers == 0) {
                    println("    (none found - maybe reached through a table)");
                }
            }

            println("  references to the address itself:");
            int refs = 0;
            for (Reference ref : getReferencesTo(addr)) {
                println("    " + ref.getReferenceType() + " from " + ref.getFromAddress());
                refs++;
            }
            if (refs == 0) {
                println("    (none)");
            }

            println("  disassembly around it:");
            Instruction ins = getInstructionAt(addr);
            if (ins == null) {
                println("    (no instruction at this address - it may be data)");
                continue;
            }
            for (int i = 0; i < WINDOW_BEFORE; i++) {
                Instruction prev = getInstructionBefore(ins.getAddress());
                if (prev == null) {
                    break;
                }
                ins = prev;
            }
            int shown = 0;
            while (ins != null && shown < WINDOW_BEFORE + WINDOW_AFTER) {
                String mark = ins.getAddress().equals(addr) ? "->" : "  ";
                println("  " + mark + " " + ins.getAddress() + "  " + ins.toString());
                ins = getInstructionAfter(ins.getAddress());
                shown++;
            }
        }
    }
}
