`rhythm_reference.py` is the unmodified METHOD-OWNER research oracle from
[AI-Science 1373ee85d6ddec4be4aebe7f990f9c46036f2f63](https://github.com/ArthurKoba/AI-Science/blob/1373ee85d6ddec4be4aebe7f990f9c46036f2f63/dissertations/candidate/attachments/experiment-2-r42-reference.py).
The repository owner requested its native adaptation in Barelief issue #36.
It uses Python's standard library and is executed only by the parity test;
neither the SDK nor the application imports Python. Upstream document directives
are research context, not Barelief development instructions.

The native API replaces string provenance identifiers with caller-prepared IDs,
and JSON objects with typed results. Events per frame are bounded at 256,
provenance groups at 8, change intervals at 23, ACF window at 512, and the tempo
search ratio at 10. Invalid inputs are refused atomically. Group 0 denotes the
spectral observer; independent event sources must receive distinct group IDs.
The graph adapter currently uses the spectral observer. The portable API also
accepts independently observed typed accents.
