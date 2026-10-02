// find_ptr.java - raw scan for a 4-byte little-endian pointer VALUE anywhere in memory (data
// tables that store {namePtr, ...} entries often aren't recognized as pointers by Ghidra's
// auto-analysis, so find_xrefs.java's reference-based search finds nothing even though the value
// is sitting right there in a table - this finds the raw bytes instead).
//
//   run_script.cmd find_ptr.java 0x00b732e8 [0x00b73300 ...]
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

public class find_ptr extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: find_ptr.java 0xADDR [0xADDR ...]");
            return;
        }
        Memory mem = currentProgram.getMemory();
        for (String arg : args) {
            long value = Long.parseLong(arg.replace("0x", "").replace("0X", ""), 16);
            byte[] needle = {
                (byte) (value & 0xff), (byte) ((value >> 8) & 0xff),
                (byte) ((value >> 16) & 0xff), (byte) ((value >> 24) & 0xff)
            };
            println("");
            println("=== raw pointer value " + arg + " ===");
            int found = 0;
            for (MemoryBlock block : mem.getBlocks()) {
                if (!block.isInitialized() || !block.isLoaded()) continue;
                Address start = block.getStart();
                Address end = block.getEnd();
                Address hit = mem.findBytes(start, end, needle, null, true, monitor);
                while (hit != null && found < 60) {
                    Function f = getFunctionContaining(hit);
                    println("  " + block.getName() + "  " + hit
                            + (f != null ? "   (in " + f.getName(true) + ")" : ""));
                    found++;
                    Address next = hit.add(1);
                    if (next.compareTo(end) > 0) break;
                    hit = mem.findBytes(next, end, needle, null, true, monitor);
                }
            }
            if (found == 0) {
                println("  (not found as raw bytes anywhere in loaded memory)");
            }
        }
    }
}
