set term png
set output "size.png"
set   autoscale                        # scale axes automatically
unset log                              # remove any log-scaling
unset label                            # remove any previous labels
set xtic auto                          # set xtics automatically
set ytic auto                          # set ytics automatically
set title "Distribution for request size"
set xlabel "size(KB)"
set ylabel "Request Numbers"
plot	"10f.csv" using 1:2 title 'front' lt rgb "black" with linespoints,\
plot	"10b.csv" using 1:2 title 'back' lt rgb "blue" with linespoints

set term png
set output "offset.png"
set title "Distribution for locality"
set xlabel "Offset(MB)"
set ylabel "Request Numbers"
plot	"10f.csv" using 3:4 title 'front' with linespoints

set term png
set output "time.png"
set title "Distribution for arrival rate"
set xlabel "time(us)"
set ylabel "Request Numbers"
plot	"result.csv" using 5:6 title 'front' with linespoints
