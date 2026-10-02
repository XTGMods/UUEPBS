import os, shutil, subprocess, sys
# Mutation check for CharacterMemory (remembered NPCs, Character Switch Watcher slots and follower links):
# breaks one behaviour at a time and expects test_sculpt to fail. Run from this folder:  python mutate_cpp.py
# Prints CAUGHT / MISSED per mutation; PATTERN? means the code it edits has changed (update the entry).
SRC=os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
base=open(SRC+'/src/core/character_memory.cpp').read()
M=[
 ("follower not given the member's sliders", 'texts[key] = std::string("\\x01"); // force the copy below', '(void)0;'),
 ("the window's pick doesn't win", '(source.empty() || key == active)', 'source.empty()'),
 ("follower edits not shared", 'if (!source.empty())\n            {\n                p.bones = reg.edits_of(source);', 'if (false)\n            {\n                p.bones = reg.edits_of(source);'),
 ("followers restored from their own file", 'm_restored.insert(l.key); // never restored', '(void)l; // never restored'),
 ("despawned link remembered", 'it = linkedNow.count(*it) ? std::next(it) : m_linked.erase(it);', 'it = std::next(it);'),
 ("sync never skipped", 'if (!m_links_changed && bones_rev == m_sync_bones && morphs_rev == m_sync_morphs)', 'if (false)'),
 ("member without sliders doesn't take the follower's", 'if (p.text.empty() && !texts[key].empty())', 'if (false)'),
 ("outgoing not saved at once", 'persist_one(slot_of(m_player_who), slot_of(m_player_who), old.bones, old.morphs, now, Clock::duration::zero());', '(void)old;'),
]
bad=0
for name,o,n in M:
    if base.count(o)!=1: print('PATTERN?',name,base.count(o)); bad+=1; continue
    import tempfile
    d=os.path.join(tempfile.gettempdir(),'uuepbs_cmut'); shutil.rmtree(d,ignore_errors=True); shutil.copytree(SRC+'/src',d)
    open(d+'/core/character_memory.cpp','w').write(base.replace(o,n))
    exe=os.path.join(tempfile.gettempdir(),'uuepbs_ts_mut')
    if os.path.exists(exe): os.remove(exe)
    r=subprocess.run(['g++','-std=c++23','-O1','-I'+d,SRC+'/tests/test_sculpt.cpp']+[d+'/core/'+f for f in os.listdir(d+'/core') if f.endswith('.cpp')]+['-o',exe],capture_output=True,text=True)
    if r.returncode!=0: print('COMPILE?',name,r.stderr[:300]); bad+=1; continue
    r=subprocess.run([exe],cwd=SRC+'/tests',capture_output=True,text=True,timeout=300)
    fails=[l for l in r.stdout.splitlines() if l.startswith('FAIL')]
    print(('CAUGHT ' if fails else 'MISSED ')+name+(' <- '+fails[0][:100] if fails else ''))
    bad+= 0 if fails else 1
sys.exit(bad)
