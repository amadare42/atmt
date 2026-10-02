// list_blocks.java - print every memory block (name, start, end) so an address can be matched to
// its section (e.g. confirming whether a global lives in .data vs .tls).
//@category atmt
import ghidra.app.script.GhidraScript;
import ghidra.program.model.mem.MemoryBlock;

public class list_blocks extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (MemoryBlock b : currentProgram.getMemory().getBlocks()) {
            println(b.getName() + "  " + b.getStart() + " - " + b.getEnd() + "  size=0x" + Long.toHexString(b.getSize()));
        }
    }
}
