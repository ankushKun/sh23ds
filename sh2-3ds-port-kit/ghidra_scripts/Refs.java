import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.app.decompiler.*;
import java.io.*;
import java.util.*;
// args: outfile mode(addr list) ; prints referencing functions per global, decompiles those in 'dec:' list
public class Refs extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    FunctionManager fm = currentProgram.getFunctionManager();
    ReferenceManager rm = currentProgram.getReferenceManager();
    DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
    try (PrintWriter pw = new PrintWriter(a[0])) {
      for (int i=1;i<a.length;i++) {
        if (a[i].startsWith("dec:")) {
          Function f = fm.getFunctionContaining(toAddr(a[i].substring(4)));
          DecompileResults r = di.decompileFunction(f,120,monitor);
          pw.println("// ===== "+f.getName()+" @ "+f.getEntryPoint()+" size "+f.getBody().getNumAddresses());
          pw.println(r.decompileCompleted()? r.getDecompiledFunction().getC():"//fail");
          continue;
        }
        TreeSet<String> fs = new TreeSet<>();
        for (Reference r : rm.getReferencesTo(toAddr(a[i]))) { Function f = fm.getFunctionContaining(r.getFromAddress()); fs.add(f==null?"?"+r.getFromAddress():f.getEntryPoint().toString()); }
        pw.println("GLOBAL "+a[i]+" refs from "+fs.size()+" functions: "+String.join(" ",fs));
      }
    }
  }
}
