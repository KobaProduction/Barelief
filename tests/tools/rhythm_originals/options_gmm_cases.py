"""Actual GMM weighted component terms and successful Cholesky retry."""
import numpy as np

from common import Case
from model_cases import gmm_cases
from wrapper_state_cases import capture_return


def reference_components(original, values, covariance, sidecars):
    model = original.gmm.GMM(n_components=2, covariance_type=covariance)
    model.weights = sidecars[".weights"]
    model.means = sidecars[".means"]
    model.covars = sidecars[".covariances"]
    (scores, responsibilities), observed = capture_return(model.score_samples, values)
    return {"scores": scores, "responsibilities": responsibilities, "components": observed["lpr"]}


def cases(original):
    for base in gmm_cases(original):
        covariance, sidecars = base.settings["covariance"], base.sidecars

        def reference(x, covariance=covariance, sidecars=sidecars):
            return reference_components(original, x, covariance, sidecars)

        yield Case("python.madmom.options.gmm.components." + covariance, "models", "gmm", base.values,
                   [*base.arguments, 1e-7], reference,
                   {**base.settings, "weighted_component_log_density": "observed original score_samples lpr local",
                    "full_diagonal_retry": 1e-7}, sidecars)
    values = np.linspace(-.0002, .0002, 48, dtype=np.float32).reshape(24, 2)
    for kind, covariance in ((2, "tied"), (3, "full")):
        sidecars = {".weights": np.asarray([.4, .6], np.float32),
                    ".means": np.asarray([[0, 0], [.0001, -.0001]], np.float32),
                    ".covariances": np.zeros((2, 2) if kind == 2 else (2, 2, 2), np.float32)}

        def reference(x, covariance=covariance, sidecars=sidecars):
            return reference_components(original, x, covariance, sidecars)

        yield Case("python.madmom.options.gmm.cholesky_retry." + covariance, "models", "gmm", values,
                   [kind, 2, 1e-7], reference,
                   dict(covariance=covariance, components=2, features=2, fitting=False,
                        supplied_covariance="singular zero matrix", first_cholesky="fails", retry="single +1e-7 diagonal",
                        weighted_component_log_density="observed original score_samples lpr local"), sidecars)
