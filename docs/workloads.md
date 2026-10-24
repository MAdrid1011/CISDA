# Workload Inputs

## Manifest

`cisda-sim` accepts one `cisda-workload-manifest/v1` document. A manifest names
the workload and provides four paths relative to the manifest file:

```json
{
  "format": "cisda-workload-manifest/v1",
  "name": "splatam-tum-rgbd-freiburg1-desk",
  "workload": "SplaTAM",
  "compiled_workload": "../local/bundles/splatam.json",
  "native_capture": "../local/captures/splatam.jsonl.gz",
  "model_root": "../local/models/SplaTAM",
  "dataset_root": "../local/data/TUM_RGBD/rgbd_dataset_freiburg1_desk"
}
```

The model and dataset roots must exist and be nonempty. The compiled workload
and capture identities must match the workload selected by the manifest.

## Compiled Operator Workload

The compiled workload supplies the full block grid and the absolute work used
by the cycle model:

- Gaussian records and Gaussian-block relations;
- examined and blended ray operations;
- active pixels and loss work;
- Mapping optimizer objects and scalar elements;
- captured streaming block count.

For paired RGB and depth/silhouette renders, geometry-equivalent records are
merged before task construction so shared geometry work is counted once.

## Native Capture

The native capture supplies mechanism dependencies that aggregate counts do
not preserve:

- stable Gaussian IDs and exact depth-ordered point lists;
- complete tile ranges and image dimensions;
- real streaming block coordinates, valid pixels, and gradient norms;
- consumer-group and version context;
- native impact withdrawal evidence.

These records drive tile source sets, predictor queries, dependency sharing,
PIM owner selection, pending tile validation, and pose-commit eligibility.

## Included Workloads

| Manifest | Model | Dataset sequence |
| --- | --- | --- |
| `workloads/splatam.json` | SplaTAM | TUM-RGBD `rgbd_dataset_freiburg1_desk` |
| `workloads/gaussian_slam.json` | Gaussian-SLAM | TUM-RGBD `rgbd_dataset_freiburg1_desk` |
| `workloads/vtgaussian_slam.json` | VTGaussian-SLAM | TUM-RGBD `rgbd_dataset_freiburg1_desk` |

The `local/` directory is intentionally ignored by Git. Keeping the assets
there preserves the real workload inputs locally without embedding datasets,
model trees, native captures, or compiled bundles in the source repository.
