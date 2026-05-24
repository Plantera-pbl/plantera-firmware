# Soil Moisture Calibration

This project uses the soil moisture percentage as a practical watering guide, based on observed readings from the current sensor and soil.

## Current Scale

| Reading | Meaning | Action |
| --- | --- | --- |
| `< 35%` | Dry | Water the plant |
| `35-45%` | Getting dry | Watch it / water soon |
| `45-65%` | Good moisture | Do not water |
| `65-75%` | Freshly watered / wet | Do not water |
| `> 75%` | Very wet | Avoid watering |

## Observed Reference Points

| Reading | Observation |
| --- | --- |
| `30-35%` | Dry soil |
| `50-60%` | Moist / good range |
| `70%+` | Freshly watered |

## Suggested Automation Rule

Use hysteresis so the state does not flicker around one threshold:

```text
Needs water when soil moisture is below 40%.
Returns to OK after soil moisture rises above 50%.
Warn as too wet above 75%.
```

## Current Interpretation

A reading around `55%` means the soil is in the good/moist range and the plant does not need watering.
