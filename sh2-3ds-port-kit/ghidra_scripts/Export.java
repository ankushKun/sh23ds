import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.app.decompiler.*;
import java.io.*;
import java.util.*;
public class Export extends GhidraScript {
  public void run() throws Exception {
    String out = getScriptArgs()[0];
    new File(out).mkdirs();
    FunctionManager fm = currentProgram.getFunctionManager();
    ReferenceManager rm = currentProgram.getReferenceManager();
    // functions
    try (PrintWriter pw = new PrintWriter(out+"/functions.tsv")) {
      pw.println("entry\tname\tsize\tcallers\tcallees\timports\tthunk");
      for (Function f : fm.getFunctions(true)) {
        Set<String> imps = new TreeSet<>();
        Set<Function> callees = f.getCalledFunctions(monitor);
        for (Function c : callees) { Function t = c.isThunk()? c.getThunkedFunction(true):c; if (t!=null && t.isExternal()) imps.add(t.getParentNamespace().getName()+"!"+t.getName()); }
        pw.println(f.getEntryPoint()+"\t"+f.getName()+"\t"+f.getBody().getNumAddresses()+"\t"+f.getCallingFunctions(monitor).size()+"\t"+callees.size()+"\t"+String.join(",",imps)+"\t"+f.isThunk());
      }
    }
    // xrefs to imports
    try (PrintWriter pw = new PrintWriter(out+"/import_xrefs.tsv")) {
      for (Function ext : fm.getExternalFunctions()) {
        for (Reference r : rm.getReferencesTo(ext.getEntryPoint())) { }
        Address[] thunks = ext.getFunctionThunkAddresses(true);
        List<Address> targets = new ArrayList<>(); targets.add(ext.getEntryPoint());
        if (thunks!=null) targets.addAll(Arrays.asList(thunks));
        for (Symbol s : currentProgram.getSymbolTable().getSymbols(ext.getName())) targets.add(s.getAddress());
        Set<String> seen = new HashSet<>();
        for (Address a : targets) for (Reference r : rm.getReferencesTo(a)) {
          Function caller = fm.getFunctionContaining(r.getFromAddress());
          String k = ext.getParentNamespace().getName()+"!"+ext.getName()+"\t"+r.getFromAddress()+"\t"+(caller==null?"?":caller.getEntryPoint().toString());
          if (seen.add(k)) pw.println(k);
        }
      }
    }
    // decompile requested
    String[] args = getScriptArgs();
    if (args.length>1) {
      DecompInterface di = new DecompInterface(); di.openProgram(currentProgram);
      try (PrintWriter pw = new PrintWriter(out+"/decomp.c")) {
        for (int i=1;i<args.length;i++) {
          Function f = fm.getFunctionContaining(toAddr(args[i]));
          if (f==null) { pw.println("// no function at "+args[i]); continue; }
          DecompileResults res = di.decompileFunction(f, 120, monitor);
          pw.println("// ===== "+f.getName()+" @ "+f.getEntryPoint()+" size "+f.getBody().getNumAddresses());
          pw.println(res.decompileCompleted()? res.getDecompiledFunction().getC() : "// failed: "+res.getErrorMessage());
        }
      }
    }
  }
}
