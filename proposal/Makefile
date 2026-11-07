# proposal.tex -> proposal.pdf
#
#   make            the proposal
#   make clean      remove the intermediate files
#
# Uses tectonic when it is installed (it fetches missing packages itself),
# otherwise latexmk with XeLaTeX and BibTeX. refs.bib holds the 30 cited
# entries, copied from literature/refs.bib, which is not in the repository;
# this directory builds on its own.

ifneq ($(shell command -v tectonic 2>/dev/null),)
TEX = tectonic --keep-logs
else ifneq ($(shell command -v latexmk 2>/dev/null),)
TEX = latexmk -xelatex -interaction=nonstopmode -halt-on-error
else
TEX = $(error needs tectonic or latexmk)
endif

.PHONY: all clean
.DELETE_ON_ERROR:

all: proposal.pdf

proposal.pdf: proposal.tex refs.bib
	$(TEX) proposal.tex

clean:
	rm -f *.aux *.bbl *.blg *.log *.out *.fls *.fdb_latexmk *.xdv
