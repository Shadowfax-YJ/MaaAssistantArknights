# Short item identities

The two JPG files are unchanged six-slot inventory screenshots from the six
v1.1.22 runs audited for seed-pod / seed confusion. `sources.json` records each
source archive member and SHA-256. Both pages visibly contain one seed and one
seed pod. The native replay runs the production OCR model and resource task,
then removes seed pod from the vocabulary to verify that an unknown identity
is rejected instead of becoming a second seed.

`matching-cases.json` contains controlled OCR text, not claims about additional
game items or historical model outputs. Native OCRer postprocessing and the
independent analysis catalogue share its part cases; analysis also checks its
collectible cases. Exact short names, known normalization and long-name error
tolerance remain covered. The analysis copy is byte-identical.

The current policy supersedes the earlier blanket triple restriction: two-character names remain exact; three-character names with no one-edit neighbour regain unique correction. Neighboring triples, cross-length neighbours and ambiguous readings stay rejected.
