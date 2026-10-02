// dump_name_table.java - walk a {char* name; int32 value;}[] array (null name terminates) and
// print each pair, resolving the name pointer to its string.
//
//   run_script.cmd dump_name_table.java 0x00b86508
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;

public class dump_name_table extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: dump_name_table.java 0xADDR [maxEntries]");
            return;
        }
        Address addr = toAddr(Long.parseLong(args[0].replace("0x", ""), 16));
        int max = args.length > 1 ? Integer.parseInt(args[1]) : 64;
        Memory mem = currentProgram.getMemory();
        for (int i = 0; i < max; i++) {
            Address entry = addr.add((long) i * 8);
            int namePtr = mem.getInt(entry);
            int value = mem.getInt(entry.add(4));
            if (namePtr == 0) {
                println("  [" + i + "] (null name - end of table)");
                break;
            }
            Address nameAddr = toAddr(namePtr & 0xffffffffL);
            StringBuilder sb = new StringBuilder();
            try {
                for (int j = 0; j < 64; j++) {
                    byte b = mem.getByte(nameAddr.add(j));
                    if (b == 0) break;
                    sb.append((char) (b & 0xff));
                }
            } catch (Exception e) {
                sb.append("<unreadable@").append(nameAddr).append(">");
            }
            println("  [" + i + "] \"" + sb + "\" -> value=" + value + " (0x" + Integer.toHexString(value) + ")");
        }
    }
}
