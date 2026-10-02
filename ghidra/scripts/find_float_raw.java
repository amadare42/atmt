// find_float_raw.java - scan raw memory bytes for a float32 bit pattern (Ghidra data typing
// aside), and list references to any address where it occurs.
//
//   run_script.cmd find_float_raw.java 1280.0 720.0
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class find_float_raw extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: find_float_raw.java <value> [value ...]");
            return;
        }
        Memory mem = currentProgram.getMemory();
        for (String arg : args) {
            float target = Float.parseFloat(arg);
            int bits = Float.floatToIntBits(target);
            byte[] pattern = new byte[] {
                (byte) (bits & 0xff), (byte) ((bits >> 8) & 0xff),
                (byte) ((bits >> 16) & 0xff), (byte) ((bits >> 24) & 0xff)
            };
            println("");
            println("=== " + arg + "  (bytes " + String.format("%02x %02x %02x %02x", pattern[0], pattern[1], pattern[2], pattern[3]) + ") ===");
            int found = 0;
            for (MemoryBlock block : mem.getBlocks()) {
                if (!block.isInitialized() || !block.isLoaded()) {
                    continue;
                }
                Address start = block.getStart();
                Address end = block.getEnd();
                Address searchFrom = start;
                while (true) {
                    Address hit = mem.findBytes(searchFrom, end, pattern, null, true, monitor);
                    if (hit == null) {
                        break;
                    }
                    found++;
                    println("  " + hit + "  in block " + block.getName());
                    int refs = 0;
                    for (Reference ref : getReferencesTo(hit)) {
                        Address from = ref.getFromAddress();
                        Function f = getFunctionContaining(from);
                        println("        ref from " + from + "  in "
                                + (f == null ? "(no function)" : f.getName(true) + " @" + f.getEntryPoint()));
                        refs++;
                        if (refs > 15) {
                            println("        ...");
                            break;
                        }
                    }
                    if (refs == 0) {
                        println("        (no references)");
                    }
                    try {
                        searchFrom = hit.add(1);
                    } catch (Exception e) {
                        break;
                    }
                    if (found > 200) {
                        println("  ...(stopping, too many hits)");
                        break;
                    }
                }
            }
            println("  total: " + found);
        }
    }
}
