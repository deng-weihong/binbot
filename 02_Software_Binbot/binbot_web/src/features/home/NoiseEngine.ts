import {
  generateNoiseSamples,
  noiseSounds,
  type WhiteNoiseSettings,
} from "../../shared/model/whiteNoise";

function browserAudioContext(): AudioContext {
  const Audio =
    window.AudioContext ??
    (window as Window & { webkitAudioContext?: typeof AudioContext })
      .webkitAudioContext;
  if (!Audio)
    throw new Error(
      "当前浏览器不支持音频试听，请使用新版 Chrome、Safari 或 Edge。",
    );
  return new Audio();
}

type Voice = {
  source: AudioBufferSourceNode;
  filter: BiquadFilterNode;
  envelope: GainNode;
  gain: GainNode;
  oscillator?: OscillatorNode;
  modulation?: GainNode;
};

/** Local audition only. Audio resources are created after an explicit play click. */
export class NoiseEngine {
  private context: AudioContext | null = null;
  private master: GainNode | null = null;
  private compressor: DynamicsCompressorNode | null = null;
  private voices: Voice[] = [];
  private wanted = false;
  private disposed = false;
  private reportedPlaying = false;
  private stopAt: number | null = null;
  private masterLevel = 0;

  constructor(
    private readonly onPlayingChange: (playing: boolean) => void,
    private readonly createContext: () => AudioContext = browserAudioContext,
  ) {}

  get playing() {
    return !this.disposed && this.wanted && this.context?.state === "running";
  }

  private reportState() {
    if (this.disposed) return;
    const playing = this.playing;
    if (playing === this.reportedPlaying) return;
    this.reportedPlaying = playing;
    this.onPlayingChange(playing);
  }

  private initialize() {
    if (this.context) return this.context;
    const context = this.createContext();
    this.context = context;
    this.master = context.createGain();
    this.master.gain.value = 0;
    this.compressor = context.createDynamicsCompressor();
    this.compressor.threshold.value = -7;
    this.compressor.knee.value = 12;
    this.compressor.ratio.value = 6;
    this.compressor.attack.value = 0.005;
    this.compressor.release.value = 0.25;
    this.master.connect(this.compressor);
    this.compressor.connect(context.destination);
    const startAt = context.currentTime;
    for (const [index, sound] of noiseSounds.entries()) {
      const source = context.createBufferSource();
      const buffer = context.createBuffer(
        1,
        context.sampleRate * 6,
        context.sampleRate,
      );
      buffer
        .getChannelData(0)
        .set(
          generateNoiseSamples(
            sound.kind,
            buffer.length,
            0x9e3779b9 ^ Math.imul(0x85ebca6b, index + 1),
          ),
        );
      source.buffer = buffer;
      source.loop = true;
      const filter = context.createBiquadFilter();
      filter.type = sound.filter;
      filter.frequency.value = sound.frequency;
      filter.Q.value = sound.q;
      const envelope = context.createGain();
      envelope.gain.value = 1 - sound.depth / 2;
      const gain = context.createGain();
      gain.gain.value = 0;
      const voice: Voice = { source, filter, envelope, gain };
      this.voices.push(voice);
      source.connect(filter);
      filter.connect(envelope);
      envelope.connect(gain);
      gain.connect(this.master);
      if (sound.lfo > 0) {
        voice.oscillator = context.createOscillator();
        voice.oscillator.frequency.value = sound.lfo;
        voice.modulation = context.createGain();
        voice.modulation.gain.value = sound.depth / 2;
        voice.oscillator.connect(voice.modulation);
        voice.modulation.connect(envelope.gain);
        voice.oscillator.start(startAt);
      }
      source.start(startAt);
    }
    context.onstatechange = () => this.reportState();
    return context;
  }

  update(settings: WhiteNoiseSettings) {
    if (!this.context || this.disposed) return;
    const time = this.context.currentTime;
    this.voices.forEach((voice, index) =>
      voice.gain.gain.setTargetAtTime(
        settings.levels[noiseSounds[index].id] / 100,
        time,
        0.035,
      ),
    );
    this.masterLevel = (settings.master / 100) * 0.65;
    this.scheduleMaster();
  }

  private scheduleMaster() {
    if (!this.context || !this.master) return;
    const time = this.context.currentTime;
    const gain = this.master.gain;
    gain.cancelScheduledValues(time);
    gain.setTargetAtTime(this.wanted ? this.masterLevel : 0, time, 0.035);
    // Audio-clock scheduling keeps the timer quiet even in a throttled background tab.
    if (this.wanted && this.stopAt !== null)
      gain.setValueAtTime(0, Math.max(time, this.stopAt));
  }

  setStopAfter(seconds: number | null) {
    if (!this.context || this.disposed) return;
    this.stopAt =
      seconds === null ? null : this.context.currentTime + Math.max(0, seconds);
    this.scheduleMaster();
  }

  async play(settings: WhiteNoiseSettings): Promise<boolean> {
    if (this.disposed) return false;
    this.wanted = true;
    this.stopAt = null;
    try {
      const context = this.initialize();
      this.update(settings);
      await context.resume();
      if (this.disposed) return false;
      if (!this.wanted) {
        await context.suspend();
        return false;
      }
      if (context.state !== "running")
        throw new Error("浏览器暂未允许播放音频，请再次点击试听。");
      this.reportState();
      return true;
    } catch (error) {
      this.wanted = false;
      this.reportState();
      if (!this.disposed) {
        this.scheduleMaster();
        await this.context?.suspend().catch(() => {});
        throw error;
      }
      return false;
    }
  }

  pause() {
    this.wanted = false;
    this.stopAt = null;
    this.reportState();
    if (this.context && this.context.state !== "closed") {
      this.scheduleMaster();
      void this.context.suspend().catch(() => {});
    }
  }

  dispose() {
    this.disposed = true;
    this.wanted = false;
    if (this.context) this.context.onstatechange = null;
    for (const voice of this.voices) {
      voice.source.stop();
      voice.oscillator?.stop();
      voice.source.disconnect();
      voice.filter.disconnect();
      voice.envelope.disconnect();
      voice.gain.disconnect();
      voice.oscillator?.disconnect();
      voice.modulation?.disconnect();
    }
    this.voices = [];
    this.master?.disconnect();
    this.compressor?.disconnect();
    if (this.context && this.context.state !== "closed")
      void this.context.close().catch(() => {});
  }
}
