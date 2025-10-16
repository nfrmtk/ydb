perf record -o ~/.join_perf/perfdata/perf.$alg.hotspot-like.data -F 10000 --call-graph dwarf --sample-cpu ./join_perf --preset $alg --key-type int --preset-flavour same-size
perf script -i ~/.join_perf/perfdata/perf.$alg.hotspot-like.data > ./perf.$alg.hotspot-like.txt
~/arcadia/ya upload ./perf.$alg.hotspot-like.txt