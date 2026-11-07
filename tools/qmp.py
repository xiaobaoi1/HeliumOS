import socket,json,sys,time
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);s.connect('/tmp/qmp.sock');f=s.makefile('rwb',buffering=0)
def rx():
 while True:
  l=f.readline()
  if not l:return None
  o=json.loads(l)
  if 'event' not in o:return o
rx();f.write(b'{"execute":"qmp_capabilities"}\n');rx()
c=sys.argv[1]
def send(o):f.write(json.dumps(o).encode()+b'\n');rx()
if c=='shot': send({'execute':'human-monitor-command','arguments':{'command-line':'screendump /tmp/wm.ppm'}})
elif c=='rel':
 dx,dy=int(sys.argv[2]),int(sys.argv[3]);e=[]
 if dx:e.append({'type':'rel','data':{'axis':'x','value':dx}})
 if dy:e.append({'type':'rel','data':{'axis':'y','value':dy}})
 send({'execute':'input-send-event','arguments':{'events':e}})
elif c=='btn':
 d=sys.argv[2];send({'execute':'input-send-event','arguments':{'events':[{'type':'btn','data':{'down':d=='down','button':'left'}}]}})
elif c=='hmp': send({'execute':'human-monitor-command','arguments':{'command-line':' '.join(sys.argv[2:])}})
elif c=='text':
 qm={' ':'spc','\n':'ret','.':'dot','/':'slash','\\':'backslash',':':'semicolon',';':'semicolon','=':'equal','-':'minus','_':'minus'}
 def key(q,d): send({'execute':'input-send-event','arguments':{'events':[{'type':'key','data':{'down':d,'key':{'type':'qcode','data':q}}}]}})
 for ch in sys.argv[2]:
  shift=ch.isupper() or ch=='_'; q=ch.lower() if ch.isalpha() else qm.get(ch,ch)
  if shift:key('shift',True)
  key(q,True);key(q,False)
  if shift:key('shift',False)
  time.sleep(.008)
