all:	main.pdf

main.pdf: $(wildcard *.tex *.bib)
	rubber -Wrefs -Wmisc --pdf main.tex

clean:
	rubber --pdf --clean main.tex

