// Names SecuROM-redirected imports in the Director's Cut, from tools/dc_imports.py output.
//
// 1. jmp [original IAT] thunks (dc_import_thunks.tsv, real code) become thunk functions
//    of the matching external, so calls to them decompile as the API name.
// 2. Dispatcher sites (call [0x24dc3f0]) and calls into the encrypted SecuROM stub
//    tables (dc_import_sites.tsv) get an EOL comment and a CALL_OVERRIDE_UNCONDITIONAL
//    reference. The stubs hold encrypted bytes until runtime, so no function is built
//    on them. The decompiler only honours overrides to memory addresses, so each API
//    gets a small thunk (jmp [real IAT slot]) in a synthetic block "sh23ds_imp" at
//    0x2600000. That block is not part of the exe; the exe's own bytes are never changed.
//
// Args: <dc_import_sites.tsv> <dc_import_thunks.tsv>
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.nio.file.*;
import java.util.*;

public class ApplyImportSites extends GhidraScript {
  static final long BLOCK = 0x2600000L;
  Map<String, Function> ext = new HashMap<>();
  Map<String, Address> thunkFor = new HashMap<>();
  MemoryBlock blk;
  int nextSlot = 0;

  Function external(String imp) {
    String sym = imp.substring(imp.indexOf('!') + 1);
    if (sym.equals("DirectSoundCreate")) sym = "Ordinal_11";
    return ext.get(sym);
  }

  // The loaded IAT slot (second .idata) that points at this external.
  Address iatSlot(Function f) {
    for (Reference r : currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint())) {
      MemoryBlock b = getMemoryBlock(r.getFromAddress());
      if (b != null && !b.isExecute()) return r.getFromAddress();
    }
    return null;
  }

  Function makeThunk(Address a, Function target) throws Exception {
    if (getInstructionAt(a) == null) disassemble(a);
    Function f = getFunctionAt(a);
    if (f == null) f = createFunction(a, null);
    else if (f.getBody().getNumAddresses() <= 1) CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
    if (f != null && target != null && f.getThunkedFunction(false) != target) f.setThunkedFunction(target);
    return f;
  }

  Address syntheticThunk(Function target) throws Exception {
    String key = target.getName(true);
    Address t = thunkFor.get(key);
    if (t != null) return t;
    Memory mem = currentProgram.getMemory();
    if (blk == null) {
      blk = mem.getBlock("sh23ds_imp");
      if (blk == null) {
        blk = mem.createInitializedBlock("sh23ds_imp", toAddr(BLOCK), 0x1000, (byte) 0xCC, monitor, false);
        blk.setPermissions(true, false, true);
        blk.setComment("synthetic import thunks for SecuROM dispatcher sites; not in sh2pc.exe");
      }
    }
    t = toAddr(BLOCK + 8L * nextSlot++);
    Address slot = iatSlot(target);
    clearListing(t, t.add(7));
    if (slot != null) {
      long s = slot.getOffset();
      setBytes(t, new byte[] {(byte) 0xFF, 0x25, (byte) s, (byte) (s >> 8), (byte) (s >> 16), (byte) (s >> 24), (byte) 0xCC, (byte) 0xCC});
    } else {
      setByte(t, (byte) 0xC3);
    }
    Function f = getFunctionAt(t);
    if (f != null) removeFunction(f);
    makeThunk(t, target);
    thunkFor.put(key, t);
    return t;
  }

  public void run() throws Exception {
    String[] args = getScriptArgs();
    // External functions, including demangled C++ ones in class namespaces...
    for (Function f : currentProgram.getFunctionManager().getExternalFunctions()) {
      ext.putIfAbsent(f.getName(), f);
      ExternalLocation loc = f.getExternalLocation();
      if (loc != null && loc.getOriginalImportedName() != null) ext.putIfAbsent(loc.getOriginalImportedName(), f);
    }
    // ...plus external labels made into functions (SecuROM never calls e.g. Direct3DCreate8
    // or dsound Ordinal_11 through the IAT, so Ghidra left them labels).
    ExternalManager em = currentProgram.getExternalManager();
    for (String lib : em.getExternalLibraryNames()) {
      ExternalLocationIterator it = em.getExternalLocations(lib);
      while (it.hasNext()) {
        ExternalLocation loc = it.next();
        Function f = loc.isFunction() ? loc.getFunction() : loc.createFunction();
        if (f == null) continue;
        ext.putIfAbsent(f.getName(), f);
        if (loc.getOriginalImportedName() != null) ext.putIfAbsent(loc.getOriginalImportedName(), f);
      }
    }

    // 1. jmp thunks (stubs are encrypted until runtime; their call sites are handled in 2)
    int thunks = 0, noExtThunk = 0;
    for (String line : Files.readAllLines(Paths.get(args[1]))) {
      String[] p = line.split("\t", -1);
      if (p[0].equals("addr") || !p[1].equals("jmp_thunk")) continue;
      Function target = external(p[2]);
      if (target == null) { noExtThunk++; continue; }
      if (makeThunk(toAddr(p[0]), target) != null) thunks++;
    }

    // 2. dispatcher sites
    ReferenceManager rm = currentProgram.getReferenceManager();
    Listing listing = currentProgram.getListing();
    int comments = 0, overrides = 0, noExt = 0;
    Set<String> missing = new TreeSet<>();
    for (String line : Files.readAllLines(Paths.get(args[0]))) {
      String[] p = line.split("\t", -1);
      if (p[0].equals("site") || p[3].isEmpty()) continue;
      Address a = toAddr(p[0]);
      listing.setComment(a, CommentType.EOL, "SecuROM -> " + p[3]);
      comments++;
      Function target = external(p[3]);
      if (target == null) { noExt++; missing.add(p[3]); continue; }
      if (listing.getInstructionAt(a) == null) disassemble(a);
      for (Reference r : rm.getReferencesFrom(a)) {
        if (r.getReferenceType() == RefType.CALL_OVERRIDE_UNCONDITIONAL) rm.delete(r);
      }
      Reference r = rm.addMemoryReference(a, syntheticThunk(target), RefType.CALL_OVERRIDE_UNCONDITIONAL, SourceType.USER_DEFINED, 0);
      rm.setPrimary(r, true);
      overrides++;
    }
    println("thunk functions " + thunks + " (no external " + noExtThunk + "), comments " + comments
            + ", call overrides " + overrides + " via " + thunkFor.size() + " synthetic thunks, no external " + noExt + " " + missing);
  }
}
