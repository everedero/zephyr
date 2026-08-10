.. zephyr:code-sample:: codec-tone
   :name: audio codec tone

   Play a test tone, TX only, at every sample rate and bit depth the codec accepts.

Overview
********

Playback only: no microphone, no loopback, no PCM blob. The sample generates a
sine wave itself, discovers what the codec supports, and plays the tone at each
supported combination in turn.

The audio codec API has no capability query, so support is discovered by
**probing**: :c:func:`audio_codec_configure` is called for every value of
:c:enum:`audio_pcm_rate_t` and :c:enum:`audio_pcm_width_t`, and whatever returns
0 is taken as supported. The device is never started during the probe, so a
rejected configuration is harmless.

Configuration
*************

.. list-table::
   :header-rows: 1

   * - Option
     - Default
     - Meaning
   * - ``CONFIG_SAMPLE_TONE_FREQ_HZ``
     - 440
     - Tone frequency, played at every supported rate.
   * - ``CONFIG_SAMPLE_BLOCK_SIZE``
     - 320
     - Bytes per block. Must not exceed the codec driver's own maximum.
   * - ``CONFIG_SAMPLE_STEP_MS``
     - 2000
     - How long each rate/width combination plays.

Sample format
=============

One sample occupies ``DIV_ROUND_UP(width, 8)`` bytes: 2 for 16-bit, 3 for
20- and 24-bit, 4 for 32-bit. The value is a signed ``width``-bit number stored
little-endian and sign-extended across the whole container, so a 20-bit sample
sits sign-extended in three bytes.

Because a 3-byte container does not divide 320 evenly, the block size actually
given to the codec is rounded down to a whole number of samples. Every write is
then a full block: a short write would leave the driver to pad the tail, which
is audible as a click.

Phase is carried across blocks, so the tone is continuous and its frequency is
exact at every sample rate.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/audio/codec_tone
   :host-os: unix
   :board: nucleo_f756zg
   :goals: run
   :compact:

Sample Output
=============

.. code-block:: console

    [00:00:00.001,000] <inf> codec_tone: Audio codec tone sample
    [00:00:00.001,000] <inf> codec_tone: probing codec capabilities
    [00:00:00.001,000] <inf> codec_tone:    rate    16   20   24   32
    [00:00:00.001,000] <inf> codec_tone:    8000    ok    -    -    -
    [00:00:00.001,000] <inf> codec_tone:   11025    ok    -    -    -
    [00:00:00.001,000] <inf> codec_tone:   16000    ok    -    -    -
    ...
    [00:00:00.002,000] <inf> codec_tone: playing 440 Hz at 8000 Hz / 16 bit (320 bytes per block)
    ...
    [00:00:20.000,000] <inf> codec_tone: Exiting
