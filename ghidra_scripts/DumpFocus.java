//DumpFocus.java - Ghidra headless script: report the imported program and dump
//decompiled pseudocode for every function to a UTF-8 text file.
//@category REA
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.File;
import java.io.PrintWriter;

public class DumpFocus extends GhidraScript {
    @Override
    public void run() throws Exception {
        println("LANG " + currentProgram.getLanguageID()
                + " CSPEC " + currentProgram.getCompilerSpec().getCompilerSpecID());
        println("IMAGEBASE " + currentProgram.getImageBase());
        println("MINADDR " + currentProgram.getMinAddress() + " MAXADDR " + currentProgram.getMaxAddress());
        for (ghidra.program.model.mem.MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            println("BLOCK " + block.getName() + " " + block.getStart() + "-" + block.getEnd()
                    + " r=" + block.isRead() + " w=" + block.isWrite() + " x=" + block.isExecute());
        }
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        String[] args = getScriptArgs();
        String out = args.length > 0 ? args[0] : "decompiled.txt";
        int count = 0;
        int failed = 0;
        try (PrintWriter writer = new PrintWriter(new File(out), "UTF-8")) {
            writer.println("// language=" + currentProgram.getLanguageID()
                    + " compiler=" + currentProgram.getCompilerSpec().getCompilerSpecID()
                    + " imageBase=" + currentProgram.getImageBase());
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext() && !monitor.isCancelled()) {
                Function function = functions.next();
                count++;
                writer.println("=== " + function.getName() + " @ " + function.getEntryPoint()
                        + " size=" + function.getBody().getNumAddresses() + " ===");
                DecompileResults results = decompiler.decompileFunction(function, 60, monitor);
                if (results != null && results.decompileCompleted()) {
                    writer.println(results.getDecompiledFunction().getC());
                } else {
                    failed++;
                    writer.println("// decompile failed: "
                            + (results == null ? "no result" : results.getErrorMessage()));
                }
                writer.println();
            }
            writer.println("// total functions: " + count + ", failed: " + failed);
        }
        decompiler.dispose();
        println("FUNCTIONS " + count + " FAILED " + failed + " OUT " + out);
    }
}
