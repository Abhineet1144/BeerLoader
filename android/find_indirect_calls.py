#!/usr/bin/env python3
"""Read a clang text AST (-Xclang -ast-dump) on stdin and list every call whose callee is a function POINTER expression
(a variable, parameter, field, dereference or cast) rather than a direct function: host code that may be calling guest code.
Clang prints locations relative to the previously printed one, so file/line state is tracked across the whole dump.
Output: file:line<TAB>callee-name<TAB>callee-type"""
import re,sys
cur_file="?"; cur_line=0
indent_re=re.compile(r'^([ |`\-]*)(\w+)')
name_re=re.compile(r"(?:Var|ParmVar|Field|Function)\s+0x[0-9a-f]+\s+'(\w+)'")
mem_re=re.compile(r"(?:->|\.)(\w+)\s+0x")
loc_re=re.compile(r'<([^<>]*)>')
def update_locs(line):
    """Advance (cur_file, cur_line) through every location printed in the node's <...> range; return the begin location."""
    global cur_file,cur_line
    m=loc_re.search(line)
    begin=None
    if not m: return None
    for i,tok in enumerate(t.strip() for t in m.group(1).split(',')):
        parts=tok.split(':')
        if tok.startswith('line:') and len(parts)>=3: cur_line=int(parts[1])
        elif tok.startswith('col:'): pass
        elif len(parts)>=3 and parts[1].isdigit():
            cur_file=parts[0]; cur_line=int(parts[1])
        if i==0: begin=(cur_file,cur_line)
    return begin
results={}
stack=[]
def flush_until(indent):
    while stack and stack[-1]['done_indent'] is not None and indent<=stack[-1]['done_indent']:
        c=stack.pop(); results.setdefault(c['loc'],(c['name'] or '?',c['type']))
for raw in sys.stdin:
    line=raw.rstrip('\n')
    m=indent_re.match(line)
    if not m: continue
    indent=len(m.group(1)); kind=m.group(2)
    begin=update_locs(line)
    flush_until(indent)
    if stack and stack[-1]['callee_indent'] is None and indent>stack[-1]['indent']:
        c=stack[-1]
        if 'FunctionToPointerDecay' in line or 'BuiltinFnToFnPtr' in line:
            stack.pop()
        else:
            c['callee_indent']=indent; c['done_indent']=indent
            tm=re.search(r"'([^']*\*[^']*)'",line); c['type']=tm.group(1) if tm else ''
    elif stack and stack[-1]['callee_indent'] is not None and indent>stack[-1]['callee_indent'] and not stack[-1]['name']:
        nm=name_re.search(line) or mem_re.search(line)
        if nm: stack[-1]['name']=nm.group(1)
    if kind=='CallExpr' and begin:
        stack.append({'indent':indent,'loc':begin,'callee_indent':None,'done_indent':None,'name':None,'type':''})
flush_until(-1)
for (f,l),(n,t) in sorted(results.items()): print(f"{f}:{l}\t{n}\t{t}")
print("indirect calls:",len(results),file=sys.stderr)
