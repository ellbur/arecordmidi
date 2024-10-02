
Modify ALSA’s [`arecordmidi`](https://github.com/alsa-project/alsa-utils/blob/master/seq/aplaymidi/arecordmidi.c) to add useful features:

- [ ] Exit when device disconnected.
- [ ] Write file immediately rather when exiting.

Due to how midi files are organized, the following features must be removed:

- Multiple ports
- Metronome

