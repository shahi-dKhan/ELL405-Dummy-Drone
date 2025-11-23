We're not building a flying drone — it’s a dummy drone, i.e., a computational model simulating the control, perception, and logging tasks typical of a real drone.
So our real goal is to measure timing behavior under different scheduling policies — not to make it fly.


Our statement can be rearticulated as: A distributed real-time testbed to benchmark SCHED_FIFO/RR vs custom RM/EDF schedulers for multi-deadline workloads in containerized environments


| Component                  | Deadline Type   | Description                                                                                                                                                      | Why it belongs on the Pi                                             |
| -------------------------- | --------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| **Control Loop**           | Hard (≈ 20 ms)  | Updates (x, y, z) position and simulates attitude control; publishes `/position` and `/imu`.                                                                     | Needs tight deterministic timing (main RT load).                     |
| **Camera/Perception Task** | Firm (≈ 100 ms) | Captures frames from **Pi Camera (CSI port)**, performs lightweight obstacle detection (e.g., color thresholding or motion cue). Publishes `/obstacle_detected`. | Uses direct hardware I/O; image capture latency is hardware-coupled. |
| **Logger Task**            | Soft (≈ 500 ms) | Logs telemetry and publishes `/log`.                                                                                                                             | Low priority, can run asynchronously.                                |


