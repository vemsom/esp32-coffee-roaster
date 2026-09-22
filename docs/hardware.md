# Hardvaruanteckningar

## Bas-maskin
Popcornmaskin (samma familj som i https://www.youtube.com/watch?v=U9_8eVlJ1SI)

## Flaktmotor
- 24V DC (bekraftat matt av Fredrik)
- Original-kretsen anvander varmeelementet som resistiv spanningsdelare + 4-diodsbrygga + 2 drosslar for att generera lagspanning till motorn.
- VIKTIGT: original-kretsen ar INTE galvaniskt isolerad fran natspanning trots lag uppmatt spanning. Motorn ska matas fran en egen isolerad DC-PSU i den har ombyggnaden, inte fran original-kretsen.

## Temperatursensorer
- 2x MAX6675 + K-type termoelement inkopta (levereras separat)
- Inget inbyggt felkodsstod i MAX6675 - bygg sanity-check i firmware (se docs/notes.md)

## SSR
- Time-proportioning-styrning, ej snabb PWM (skonsammare mot zero-cross SSR)
