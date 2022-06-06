from typing import Optional, Dict, List, Union, TYPE_CHECKING

from ray.actor import ActorHandle
from ray.air.config import DatasetConfig
from ray.air.preprocessor import Preprocessor

if TYPE_CHECKING:
    from ray.data import Dataset, DatasetPipeline


class _DataParallelIngestSpec:
    """Implements the execution of DatasetConfig preprocessing and ingest."""

    def __init__(self, dataset_config: Dict[str, DatasetConfig]):
        """Construct an ingest spec.

        Args:
            dataset_config: The merged default + user config dict for the trainer
                with all defaults filled in.
        """
        self.dataset_config = dataset_config
        self.preprocessed_datasets: Optional[Dict[str, "Dataset"]] = None

    def preprocess_datasets(
        self, prep: Preprocessor, datasets: Dict[str, "Dataset"]
    ) -> Dict[str, "Dataset"]:
        """Preprocess the given datasets.

        This will be called prior to `get_dataset_shards()`.

        Args:
            prep: The preprocessor to fit, if needed.
            dataset: The datasets to fit and transform.

        Returns:
            Dict of transformed datasets.
        """
        if prep:
            ds_to_fit = None
            for k, conf in self.dataset_config.items():
                if k not in datasets:
                    assert not conf.required, "Missing dataset post-validation"
                    continue
                if conf.fit:
                    ds_to_fit = datasets[k]
            if ds_to_fit:
                prep.fit(ds_to_fit)
            new_datasets = {}

            for key, dataset in datasets.items():
                if self._config(key).transform:
                    new_datasets[key] = prep.transform(dataset)
                else:
                    new_datasets[key] = dataset
        else:
            new_datasets = datasets
        self.preprocessed_datasets = new_datasets
        return new_datasets

    def get_dataset_shards(
        self, training_worker_handles: List[ActorHandle]
    ) -> List[Dict[str, Union["Dataset", "DatasetPipeline"]]]:
        """Get the shards to pass to training workers.

        Note: this has to match the signature of DatasetSpec in legacy train.

        Args:
            training_worker_handles: Actor handles of the workers, which can be used
                for locality-aware splitting.

        Returns:
            List of dataset shard dicts, one for each training worker.
        """
        dataset_dict_splits = [{} for _ in range(len(training_worker_handles))]

        for key, dataset in self.preprocessed_datasets.items():
            config = self._config(key)

            if config.streamable:
                dataset = dataset.repeat()

            if config.split:
                dataset_splits = dataset.split(
                    len(training_worker_handles),
                    equal=True,
                    locality_hints=training_worker_handles,
                )
            else:
                dataset_splits = [dataset] * len(training_worker_handles)

            for i in range(len(dataset_splits)):
                dataset_dict_splits[i][key] = dataset_splits[i]

        return dataset_dict_splits

    def _config(self, key: str) -> "DatasetConfig":
        """Get the dataset config for the given dataset name."""
        if key in self.dataset_config:
            return self.dataset_config[key]
        return self.dataset_config["*"]
