
Modify ALSA’s [`arecordmidi`](https://github.com/alsa-project/alsa-utils/blob/master/seq/aplaymidi/arecordmidi.c) to add useful features:

- [ ] Exit when device disconnected.
- [x] Build file progressively so you don't have to wait for the program to exit to get some data recorded.

Due to how midi files are organized, the following features must be removed:

- Multiple ports
- Metronome

