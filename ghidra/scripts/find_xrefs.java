// find_xrefs.java - who reads/writes a data address?
//
//   run_script.cmd find_xrefs.java 0x00c3e864 0x00c3e868
//
// Written for the "-al" mod: the save subsystem keeps its state in globals (a slot index that
// starts at -1, "busy" flags, a manager pointer). The functions that write those globals are the
// ones that *request* a load - which is what the mod has to do instead of driving the menu.
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class find_xrefs extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: find_xrefs.java 0xADDR [0xADDR ...]");
            return;
        }
        for (String arg : args) {
            Address addr = toAddr(Long.parseLong(arg.replace("0x", ""), 16));
            println("");
            println("=== references to " + arg + " ===");
            int count = 0;
            for (Reference ref : getReferencesTo(addr)) {
                Address from = ref.getFromAddress();
                Function f = getFunctionContaining(from);
                println("  " + ref.getReferenceType() + "  from " + from + "   in "
                        + (f == null ? "(no function)" : f.getName(true) + " @" + f.getEntryPoint()));
                count++;
            }
            if (count == 0) {
                println("  (none)");
            }
        }
    }
}
