DIAS	:= $(wildcard *.dia)
DIAPDFS	:= $(patsubst %.dia,%.pdf,$(DIAS))

all:	$(DIAPDFS)

%.pdf: %.dia
	dia --export=.tmp.$@ $<
	pdfcrop .tmp.$@ $@
	rm -f .tmp.$@

