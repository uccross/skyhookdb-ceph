#!/usr/bin/python

import sys,getopt,os,io,re

"""
parse query result log generated by files such as qa.sh
outputs a csv file, order is below in variable "header", which it prints as first csv line.
sample valid log lines:
run-query --num-objs 10000 --pool tpc --wthreads 10 --qdepth 192 --quiet --extended-price 91400.0 --query a --use-cls selectivity-pct=1 cache=hot start=1493088002.115970619 end=1493088006.015125206 run-6 03.899154587
run-query --num-objs 10000 --pool tpc --wthreads 10 --qdepth 192 --quiet --extended-price 91400.0 --query a selectivity-pct=1 cache=hot start=1493087570.394655435 end=1493087736.134972615 run-6 0165.740317180
select count(*) from lineitem where l_extendedprice > 91400
draining ios: 192 remaining
draining ios: 138 remaining
"""

epoch = 0

def makeDict(line, debug):
    tmp = line.split()
    dict = {}
    lineitems = [x.strip("--") for x in tmp[1:len(tmp)-1]]
    if debug: print "lineitems", lineitems
    if "num-objs" in lineitems:
        dict["nobjs"] = lineitems[lineitems.index("num-objs")+1]
    if "wthreads" in lineitems:
        dict["wthreads"] = lineitems[lineitems.index("wthreads")+1]
    if "qdepth" in lineitems:
        dict["qdepth"] = lineitems[lineitems.index("qdepth")+1]
    if "query" in lineitems:
        dict["query"] = lineitems[lineitems.index("query")+1]
    if "pool" in lineitems:
        dict["pool"] = lineitems[lineitems.index("pool")+1]
    if "dir" in lineitems:
        dict["dir"] = lineitems[lineitems.index("dir")+1]
    
    if "use-cls" in lineitems:
        dict["cls"] = "use-cls"
    else:
        dict["cls"] = "no-cls"
    
    for i in lineitems:
        s = i.split("=")
        k = s[0]
        if len(s) == 2: 
            if k in ["start", "end"]:
                v = str(int(float(s[1])))
            else: 
                v = s[1]
        else: v = "" 
         
        if k.startswith("run"):
            v = k.split('-')[1]
            k = "run"
        if k not in dict:  
            dict[k] = v 

    if "resp-time" not in dict:
        dict["resp-time"] = str(int((float(dict["end"]) - float(dict["start"]))))
    if debug: print "dict", dict
    
    global epoch
    if epoch == 0:
        epoch = int(dict["start"]) 
    dict["offset-from-epoch-qstart"] = str(int(dict["start"]) - epoch)
    dict["offset-from-epoch-qend"] = str(int(dict["end"]) - epoch)
    return dict
        
def usage():
    print "usage: ./this.py --input <filename> [--query <a|b|...>)] [--epoch <epoch start> [--debug]"
    sys.exit(2)
    
    
def main(argv):
    
    infile = ""
    debug = False
    try:
        opts, args = getopt.getopt(
            argv,
            "di:q:e:",
            ["debug","input=","query=","epoch="])            
    except getopt.GetoptError:
        usage()
        
    global epoch_start 
    epoch_start = 0
    
    for opt, arg in opts:
        if opt in ("-d", "--debug"):
            debug = True
        elif opt in ("-i", "--input"):
            infile = arg
        elif opt in ("-q", "--query"):
            query = arg        
        elif opt in ("-e", "--epoch"):
            epoch_start = int(arg)
    
    if not infile:
        usage()
        
   # header = ["filename", "nosds", "nobjs","query", "run", "time", "pool", "qdepth","wthreads", "selectivity-pct", "cls", "cache", "dir"]
    header = ["filename", "nosds", "nobjs","query", "selectivity-pct", "cls", "cache", "start", "end", "resp-time", "offset-from-epoch-qstart", "offset-from-epoch-qend"]
    print ",".join(header)
    
    with open(infile) as f:
        for line in f:
            if line.startswith("run-query"):
                if debug: 
                    print "line=%s" % line.strip()                
                results = makeDict(line, debug)
                results["filename"] = f.name
                m = re.search(r"(\d+)osd.+", f.name)
                if m: 
                    results["nosds"] = m.group(1)
                else: 
                    results["nosds"] = f.name.rstrip("osds.log")
                data = ",".join(results[k] for k in header)
                print data
                
            
if __name__ == "__main__":
    main(sys.argv[1:])