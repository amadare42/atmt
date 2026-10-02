// dump_range.java - disassemble a range, for functions longer than dump_at's window.
//
//   run_script.cmd dump_range.java 0x00485000 0x180
//
// Used for the "-al" mod: the save reader (0x00485000) is long, and what it does *after* reading
// the file decides whether calling it is the whole "load a slot" operation.
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class dump_range extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: dump_range.java 0xADDR [instruction count]");
            return;
        }
        final int count = args.length > 1 ? Integer.parseInt(args[1]) : 128;
        Address addr = toAddr(Long.parseLong(args[0].replace("0x", ""), 16));
        Function func = getFunctionContaining(addr);
        println("");
        println("=== from " + addr + (func != null ? "  (in " + func.getName(true) + ")" : "") + " ===");
        Instruction ins = getInstructionAt(addr);
        int shown = 0;
        while (ins != null && shown < count) {
            // mark calls and stores to globals: those are the interesting lines
            String text = ins.toString();
            String mark = text.startsWith("CALL") ? "-> " : "   ";
            println("  " + mark + ins.getAddress() + "  " + text);
            ins = getInstructionAfter(ins.getAddress());
            shown++;
        }
    }
}
