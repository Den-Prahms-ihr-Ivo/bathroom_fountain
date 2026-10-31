/*
 * mic_task – INMP441 → mic_frontend → clap_detector (CLAP-01, CLAP-14, BEH-02).
 *
 * A confirmed double clap is logged ("DOUBLE CLAP") and posted as EVT_CLAP.
 *
 * Warm-up (BEH-02), at boot and after a clap_* / mic_* setting changes:
 * 1 s with the detector off (the mic's startup transient), then 5 s with the
 * detector running but its detections ignored, so its background estimate
 * has caught up with steady noise such as a running shower.
 */
#ifndef MIC_TASK_H
#define MIC_TASK_H

/* Starts the task (core 1). Settings and events must be ready. */
void mic_task_start(void);

#endif /* MIC_TASK_H */
