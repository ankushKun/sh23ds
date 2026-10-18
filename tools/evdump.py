#!/usr/bin/env python3
# Prints a stage's events from pack/events/events.json in one line each: index, trigger, flags, result.
import json, sys
d = json.load(open(sys.argv[1]))
want = [int(x) for x in sys.argv[2:]]
for st in d['stages']:
    if st['id'] not in want:
        continue
    print(f"== stage {st['id']} {st['name']}  progs {st.get('ev_prog_count')}")
    for e in st['events']:
        t = e['trigger']; r = e['result']; fl = e['flags']
        req = ' '.join(f"{'' if q['state'] else '!'}{q['flag']}" for q in fl['require'])
        on = ' '.join(f"{'+' if q.get('state', True) else '-'}{q['flag']}" for q in fl.get('on_exec', []))
        sh = t.get('shape') or {}
        room = (sh.get('room') or {}).get('map', '')
        item = t.get('item')
        rs = r['type_name']
        if r['type_name'] in ('door', 'door_program'):
            dd = r['dest']
            rs += f" -> st{dd.get('stage')} {(dd.get('room') or {}).get('map','')}"
        if 'program' in r and r['program'] is not None:
            rs += f" prog{r['program']}"
        if r.get('message') is not None:
            rs += f" msg{r['message']}"
        if r.get('sets_flag'):
            rs += f" set{r['sets_flag']}"
        print(f"{e['index']:3d} {t['kind_name']:<18} {('item%s ' % item) if item else ''}{room:<6} req[{req}] exec[{on}] {rs}")
