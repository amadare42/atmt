// find_strings.java - locate the game's save subsystem by the strings it uses.
//
//   run_script.cmd find_strings.java save sdslot thumb
//
// For every defined string containing one of the arguments it prints the string, its address and
// the function at each reference. That is how the save code is found without guessing: the code
// that builds "…/autosave%02d.dat" or reads "sdslot.dat" names itself.
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class find_strings extends GhidraScript {

    private static final int MAX_PER_PATTERN = 20;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("usage: find_strings.java <substring> [substring ...]");
            return;
        }
        int[] shown = new int[args.length];
        int[] total = new int[args.length];

        DataIterator it = currentProgram.getListing().getDefinedData(true);
        while (it.hasNext()) {
            Data data = it.next();
            Object value = data.getValue();
            if (value == null) {
                continue;
            }
            String text = value.toString();
            if (text.length() < 4) {
                continue;
            }
            String lower = text.toLowerCase();
            for (int i = 0; i < args.length; i++) {
                if (!lower.contains(args[i].toLowerCase())) {
                    continue;
                }
                total[i]++;
                if (shown[i] >= MAX_PER_PATTERN) {
                    continue;
                }
                shown[i]++;
                String snippet = text.length() > 60 ? text.substring(0, 60) + "..." : text;
                println("");
                println("  [" + args[i] + "] " + data.getAddress() + " : \"" + snippet + "\"");
                int refs = 0;
                for (Reference ref : getReferencesTo(data.getAddress())) {
                    Address from = ref.getFromAddress();
                    Function f = getFunctionContaining(from);
                    println("      "
                            + (f == null ? "(no function)" : f.getName(true) + " @" + f.getEntryPoint())
                            + "   use at " + from);
                    refs++;
                }
                if (refs == 0) {
                    println("      (no references)");
                }
            }
        }
        println("");
        for (int i = 0; i < args.length; i++) {
            println("  total \"" + args[i] + "\": " + total[i]
                    + (total[i] > shown[i] ? " (showed first " + shown[i] + ")" : ""));
        }
    }
}
