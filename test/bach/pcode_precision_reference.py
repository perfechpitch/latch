"""NumPy reference for declared routing precision, independent of latch execution.

Routing is configuration, not numerical truth: every source is independently
computed, and a symbolic coverage check requires exactly one contribution from
every intended TP source. Neither packets nor simulator-produced values are read.
"""
from collections import Counter, defaultdict
import json
import numpy as np


def bf16_round(value):
    value=np.asarray(value,np.float32)
    bits=value.view(np.uint32)
    bits=((bits+0x7fff+((bits>>16)&1))>>16).astype(np.uint16)
    return (bits.astype(np.uint32)<<16).view(np.float32)


def adjacent(chip,core,direction):
    column=core%5
    if direction==0:return chip,core+5 if core<5 else core-5
    if direction==1:
        if column:return chip,core-1
        return (chip-4,9) if core==0 else (chip-1,4)
    if column<4:return chip,core+1
    return (chip+1,5) if core==4 else (chip+4,0)


def evaluate(nodes,parents,sources,destination):
    visiting=set();cache={}
    def visit(node):
        if node in cache:return cache[node]
        assert node not in visiting, 'cyclic numerical reduction'
        visiting.add(node)
        signals=[]
        for parent in sorted(parents.get(node,[])):
            signals.extend(visit(parent))
        if node in sources:signals.append((sources[node],Counter({node:1})))
        operation,out_fp32=nodes[node]
        if operation:
            assert signals, f'reduction has no operands: {node}'
            value=np.zeros_like(signals[0][0],dtype=np.float32);coverage=Counter()
            for part,counts in signals:
                value=(value+part).astype(np.float32);coverage.update(counts)
            if not out_fp32:value=bf16_round(value)
            signals=[(value,coverage)]
        visiting.remove(node);cache[node]=signals
        return signals
    signals=visit(destination)
    assert len(signals)==1, 'destination must receive one reduced value'
    value,coverage=signals[0]
    assert coverage==Counter({node:1 for node in sources}),f'missing/duplicated TP contribution: {coverage}'
    return value


class PrecisionRoutes:
    def __init__(self,bundle,catalog,logical):
        bad={};entries={}
        for line in (bundle/'pcode_ep6_tp8.bachir').read_text().splitlines():
            tokens=line.split()
            if not tokens:continue
            if tokens[0]=='CHIP':bad[int(tokens[1])]=int(tokens[2],0)
            if tokens[0]=='RTAB':
                nums=[int(t,0) for t in tokens[1:]]
                chip,core,path=nums[:3]
                entries[chip,core,path]=dict(path=path,operation=nums[3]==2,flow=nums[4]&7,out_fp32=bool(nums[25]))
        catalog_data=json.loads(catalog.read_text())
        self.raw=defaultdict(dict)
        for chip in range(48):
            harvests=catalog_data['chips'][f'c{chip}']['harvests']
            selected=next(h for k,h in sorted(harvests.items(),key=lambda item:int(item[0][1:]))
                          if h['disable_mask']==bad[chip])
            for key,core in selected['cores'].items():
                physical=int(key[1:])
                for entry in (core.get('entries',{}).values() if isinstance(core.get('entries'),dict) else core.get('entries',[])):
                    if entry.get('operation',0) and not entry.get('dataout',False):
                        continue  # Per-input PID entry; the local dataout PID carries the result.
                    net=entry.get('net')
                    if net is not None:self.raw[int(net)][chip,physical]=entries[chip,physical,int(entry['PathID'])]
        self.reverse={(chip,slot):(chip,core) for (chip,core),slot in logical.items()}
        requests=json.loads((catalog.parent.parent/'route_intents.json').read_text())
        self.graphs={};self.fc13={};self.fc2={}
        for net,request in enumerate(requests):
            if request['kind']!='reduce' or request['skip'] or request['step'] not in (6,10):continue
            def physical(coord):return self.reverse[coord[0]*16+coord[1],coord[2]]
            sources=[physical(c) for c in request['src_coord']]
            destination=physical(request['dst_coord'][0])
            records=dict(self.raw[net]);parents=defaultdict(list)
            pending=list(records)
            while pending:
                node=pending.pop()
                entry=records[node]
                for direction in range(3):
                    if entry['flow']>>direction&1:
                        other=adjacent(*node,direction)
                        if other not in records:
                            key=(*other,entry['path'])
                            assert key in entries, f'missing forwarding entry: net {net} {key}'
                            records[other]=entries[key]
                            pending.append(other)
            for node,entry in records.items():
                for direction in range(3):
                    if entry['flow']>>direction&1:
                        other=adjacent(*node,direction)
                        assert other in records, f'missing routing entry: net {net} {other}'
                        parents[other].append(node)
            nodes={node:(entry['operation'],entry['out_fp32']) for node,entry in records.items()}
            assert len(sources)==8 and len(set(sources))==8
            self.graphs[net]=(nodes,parents,sources,destination)
            if request['step']==6:self.fc13[sources[0][0]]=net
            else:self.fc2[sources[0][0]//8,request['src_coord'][0][2]]=net
        assert len(self.fc13)==48 and len(self.fc2)==48

    def reduce(self,net,values):
        nodes,parents,sources,destination=self.graphs[net]
        assert set(values)==set(sources)
        return evaluate(nodes,parents,values,destination)
