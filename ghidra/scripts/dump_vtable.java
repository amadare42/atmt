// dump_vtable.java - the functions a vtable points at, with jump thunks followed.
//
//   run_script.cmd dump_vtable.java 0x00b4f0bc [entries]
//
// Every call of the game goes through the ILT thunk region (0x402000-0x404000 and beyond), so a
// vtable's raw entries are thunks; this prints where each one lands, by slot offset.
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class dump_vtable extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: dump_vtable.java 0xVTABLE [entries]");
            return;
        }
        Address table = toAddr(Long.parseLong(args[0].replace("0x", ""), 16));
        int count = args.length > 1 ? Integer.parseInt(args[1]) : 32;
        println("=== vtable " + table + " ===");
        for (int i = 0; i < count; i++) {
            long raw = getInt(table.add(i * 4L)) & 0xffffffffL;
            Address entry = toAddr(raw);
            Address target = entry;
            Instruction ins = getInstructionAt(entry);
            if (ins != null && ins.getMnemonicString().equals("JMP") && ins.getFlows().length == 1) {
                target = ins.getFlows()[0];
            }
            Function f = getFunctionAt(target);
            if (f == null && getInstructionAt(target) == null) {
                println(String.format("  +0x%02x  %s  (not code - end of table)", i * 4, entry));
                break;
            }
            println(String.format("  +0x%02x  %s -> %s %s", i * 4, entry, target, f != null ? f.getName() : ""));
        }
    }
}
