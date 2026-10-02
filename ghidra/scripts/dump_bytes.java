// dump_bytes.java - raw bytes and ASCII at an address (data Ghidra did not define as a string).
//
//   run_script.cmd dump_bytes.java 0x00b3d940 64
//
// Written for the "-al" mod: the save manager formats a slot path from 0x00b3d940, but the table
// around it holds several format strings that Ghidra only partly recognises, and reading the raw
// bytes is faster than arguing with the data type.
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

public class dump_bytes extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: dump_bytes.java 0xADDR [length]");
            return;
        }
        final int length = args.length > 1 ? Integer.parseInt(args[1]) : 64;
        Address addr = toAddr(Long.parseLong(args[0].replace("0x", ""), 16));
        Memory mem = currentProgram.getMemory();
        println("");
        println("=== " + args[0] + "  (" + length + " bytes) ===");
        for (int row = 0; row < length; row += 16) {
            StringBuilder hex = new StringBuilder();
            StringBuilder ascii = new StringBuilder();
            for (int i = 0; i < 16 && row + i < length; i++) {
                int value;
                try {
                    value = mem.getByte(addr.add(row + i)) & 0xff;
                } catch (Exception e) {
                    hex.append("?? ");
                    continue;
                }
                hex.append(String.format("%02x ", value));
                ascii.append(value >= 0x20 && value < 0x7f ? (char) value : '.');
            }
            println("  " + addr.add(row) + "  " + hex + " " + ascii);
        }
    }
}
