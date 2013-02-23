for i in *.gbr; do
    name=${i/.*}
    t=${i#${name}.}
    t=${t%.gbr}
    echo $i -- $t

    case $t in
        top) t=GTL ;;
        topmask) t=GTS ;;
        topsilk) t=GTO ;;
        bottom) t=GBL ;;
        bottommask) t=GBS ;;
        bottomsilk) t=GBO ;;
        *) t="" ;;
    esac

    if [ -n "$t" ]; then
        mv $i $name.$t
    fi
done

for i in *.cnc; do
    name=${i/.cnc}
    mv ${i} ${name}.TXT
done
