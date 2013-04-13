Element["" "Lumberg1505-01" "" "" 0mm 0mm 0.0000 0.0000 0 100 ""]
(
# pins
        Pin[-1.5mm 3mm 2.2mm .6mm 1.8mm 1.05mm "tip" "1" "edge2"]
        Pin[0 0 2.2mm .6mm 1.8mm 1.05mm "sleeve" "2" "edge2"]
        Pin[1.5mm 3mm 2.2mm .6mm 1.8mm 1.05mm "ring" "3" "edge2"]

# support cutout
        Pin[2mm 7.5mm 0mm 0mm 0mm 1mm "" "" "hole"]
        Pin[2mm 8mm 0mm 0mm 0mm 1mm "" "" "hole"]
        Pin[-2mm 7.5mm 0mm 0mm 0mm 1mm "" "" "hole"]
        Pin[-2mm 8mm 0mm 0mm 0mm 1mm "" "" "hole"]


# main body
        ElementLine[1.5mm 0 2.5mm 0 10mil]
        ElementLine[2.5mm 0 2.5mm 2mm 10mil]
        ElementLine[2.5mm 4mm 2.5mm 10mm 10mil]
        ElementLine[2.5mm 10mm -2.5mm 10mm 10mil]
        ElementLine[-2.5mm 10mm -2.5mm 4mm 10mil]
        ElementLine[-2.5mm 2mm -2.5mm 0mm 10mil]
        ElementLine[-2.5mm 0 -1.5mm 0 10mil]
)
