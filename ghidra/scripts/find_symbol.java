// find_symbol.java - resolve a symbol name (e.g. a vftable) to its address, and dump the first N
// pointer-sized slots there (useful for reading a C++ vtable without GUI navigation).
//
//   run_script.cmd find_symbol.java "Phyre::PFramework::PApplication::vftable" 20
//
//@category atmt

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class find_symbol extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            println("usage: find_symbol.java <name substring> [slot count]");
            return;
        }
        String needle = args[0].toLowerCase();
        int slots = args.length > 1 ? Integer.parseInt(args[1]) : 0;

        SymbolIterator symbols = currentProgram.getSymbolTable().getAllSymbols(true);
        while (symbols.hasNext()) {
            Symbol sym = symbols.next();
            if (!sym.getName(true).toLowerCase().contains(needle)) {
                continue;
            }
            Address addr = sym.getAddress();
            println(sym.getName(true) + "  @ " + addr);
            for (int i = 0; i < slots; i++) {
                Address slotAddr = addr.add((long) i * 4);
                int val = getInt(slotAddr);
                Address target = toAddr(val & 0xffffffffL);
                Function f = getFunctionAt(target);
                println("  [" + i + "] " + slotAddr + " -> 0x" + Integer.toHexString(val)
                        + (f != null ? "   " + f.getName(true) : ""));
            }
        }
    }
}
