"""Check the exported program's EP masks against the declared pcode intent."""
import json


def audit_program(bundle,catalog_path,templates):
    records=[line.split() for line in (bundle/'pcode_ep6_tp8.bachir').read_text().splitlines()]
    rtab=[row for row in records if row and row[0]=='RTAB']
    assert not [row for row in rtab if int(row[12]) and int(row[14])]
    entries={(int(row[1]),int(row[2]),int(row[3])):row for row in rtab}
    bad={int(row[1]):int(row[2],0) for row in records if row and row[0]=='CHIP'}
    catalog=json.loads(catalog_path.read_text())
    requests=json.loads((catalog_path.parent.parent/'route_intents.json').read_text())
    rows=0;destinations=0
    for net,request in enumerate(requests):
        if request['step']!=4 or request['kind']!='broadcast':continue
        rows+=1;tray,chip,_=request['src_coord'][0];ep=(tray*16+chip)//8
        assert request['core_mask_idx']==[ep]*32
        count=0
        for chip in range(48):
            harvests=catalog['chips'][f'c{chip}']['harvests']
            selected=next(h for key,h in sorted(harvests.items(),key=lambda item:int(item[0][1:])) if h['disable_mask']==bad[chip])
            for key,core in selected['cores'].items():
                for entry in core.get('entries',[]):
                    if entry.get('net')!=net or not int(entry['directionMask'])&16:continue
                    assert entry['use_core_mask']==1 and entry['core_mask_idx']==ep
                    native=entries[chip,int(key[1:]),int(entry['PathID'])]
                    assert (int(native[12]),int(native[13]),int(native[14]))==(1,ep,0)
                    count+=1
        assert count==32;destinations+=count
    assert rows==12 and destinations==384
    # A routed off-chip broadcast must gate every B-core, including root.
    # Legacy exports retain the old path-63 stub and have no ingress record.
    ingress_records = [row for row in records if row[:2] == ['#', 'PCIE_INGRESS']]
    external_destinations = 0
    if ingress_records:
        assert len(ingress_records) == 1
        ingresses = [(net, r) for net, r in enumerate(requests)
                     if r['src_mem'] == 'DDR' and not r['skip']]
        assert len(ingresses) == 1
        net, req = ingresses[0]
        assert req['kind'] == 'broadcast' and req['src_coord'] == [[0,0,8]]
        assert req['dst_coord'] == [[row//4, row%4*4, 8] for row in range(12)]
        assert req['core_mask_idx'] == [row//2 for row in range(12)]
        for row in range(12):
            chip = row * 4
            hs = catalog['chips'][f'c{chip}']['harvests']
            h = next(h for h in hs.values() if h['disable_mask'] == bad[chip])
            landing = [(int(k[1:]), e) for k, core in h['cores'].items()
                       for e in core.get('entries', []) if e['net'] == net and e.get('l') == 8]
            assert len(landing) == 1
            core, e = landing[0]
            native = entries[chip, core, int(e['PathID'])]
            assert (int(native[12]), int(native[13]), int(native[14])) == (1, row//2, 0)
            assert int(native[4]) == 1  # transfer, no implicit reduction
            chain = [r for r in records if r[:3] == ['TCHAIN', str(chip), str(core)]]
            assert len(chain) == 2 and [int(r[3]) for r in chain] == [0,1]
            assert [int(r[-1]) for r in chain] == [0,1]  # check_flag -> broadcast END
            assert len([r for r in records if r[:3] == ['DATAIN', str(chip), str(core)]]) == 1
            external_destinations += 1
        assert external_destinations == 12
    images={row[-1] for row in records if row and row[0]=='KERNELCORE'}
    assert len(images)==855 and all((bundle/name).is_file() for name in images)
    assert len(templates)==768
    for (_,_,stage),template in templates.items():
        assert (template[0]&65535,template[0]>>16,template[7])==((4,12,393216) if stage==0 else (6,4,196608))
    return dict(rv_hex_images=855,route_entries=len(rtab),
                external_masked_bcores=external_destinations,fc13_broadcast_rows=rows,
                fc13_masked_destinations=destinations,masked_bypass_entries=0,mu_templates=768,
                fc13_local_shape=[512,768],fc2_local_shape=[768,256],
                fc13_expert_stride=393216,fc2_expert_stride=196608)
