# Packaging and publication gates. Variables and kernel build rules live in Makefile.

# Python wheels use an isolated PostgreSQL build profile so release packaging
# cannot silently inherit developer-only libraries or invalidate another gate's
# configured tree.  The recursive graph remains the single source of truth for
# transforming, compiling, and installing the kernel.
python-kernel:
	@$(MAKE) -j"$(JOBS)" BUILD_DIR="$(PYTHON_KERNEL_BUILD)" \
		EMBEDDED_KERNEL_PUBLIC_API_STATIC_LIBRARY="$(PYTHON_KERNEL_STATIC_LIBRARY)" \
		PG_CONFIGURE_ARGS="$(POSTGAMMA_PYTHON_PG_CONFIGURE_ARGS)" \
		CFLAGS="$(POSTGAMMA_PYTHON_PG_CFLAGS) -DPOSTGAMMA_REDACT_BUILD_METADATA" \
		kernel-static-library \
		"$(PYTHON_KERNEL_RESOURCE_EVIDENCE)"

$(PYTHON_WHEEL_RECEIPT): python-kernel Makefile VERSION python/build_backend.py \
		buildsys/python_wheel_receipt.py \
		python/postgamma-native.map \
		python/pyproject.toml python/README.md $(POSTGAMMA_PROJECT_LICENSE) \
		$(POSTGAMMA_PROJECT_NOTICE) $(POSTGAMMA_THIRD_PARTY_NOTICES) \
		$(POSTGAMMA_POSTGRESQL_LICENSE) $(POSTGAMMA_PGVECTOR_LICENSE) \
		$(PYTHON_PACKAGE_INPUTS)
	@$(PYTHON) buildsys/reset_directory.py \
		--path "$(PYTHON_WHEEL_DIR)" --within "$(PYTHON_PRODUCT_BUILD)"
	@mkdir -p "$(@D)"
	@POSTGAMMA_STATIC_LIBRARY="$(PYTHON_KERNEL_STATIC_LIBRARY)" \
		POSTGAMMA_STATIC_LINK_OPTIONS="$(PYTHON_KERNEL_STATIC_LINK_OPTIONS)" \
		POSTGAMMA_STATIC_RECEIPT="$(PYTHON_KERNEL_STATIC_RECEIPT)" \
		POSTGAMMA_RESOURCE_ROOT="$(PYTHON_KERNEL_RESOURCE_ROOT)" \
		$(PYTHON) python/build_backend.py \
			--wheel-dir "$(PYTHON_WHEEL_DIR)" --receipt "$@"

python-wheel: doctor-python version-check $(PYTHON_WHEEL_RECEIPT)

$(PYTHON_WHEEL_EVIDENCE): $(PYTHON_WHEEL_RECEIPT) \
		$(PYTHON_RELEASE_BASELINE) \
		buildsys/check_python_wheel.py buildsys/python_wheel_receipt.py \
		buildsys/check_documentation.py \
		docs/reference/python-examples.json \
		$(wildcard examples/python/*.py) \
		tests/python/test_api.py tests/python/integration_driver.py
	@$(PYTHON) buildsys/check_python_wheel.py \
		--wheel-dir "$(PYTHON_WHEEL_DIR)" \
		--receipt "$(PYTHON_WHEEL_RECEIPT)" \
		--baseline "$(PYTHON_RELEASE_BASELINE)" \
		--tests "$(ROOT)/tests/python" \
		--integration "$(ROOT)/tests/python/integration_driver.py" \
		--project-root "$(ROOT)" \
		--work-root "$(PYTHON_WHEEL_WORK_ROOT)" \
		--forbidden-prefix "$(ROOT)" \
		--installer archive --readelf "$(READELF)" --output "$@"

python-check: doctor-python version-check $(PYTHON_WHEEL_EVIDENCE)

python-release-wheel-check:
	@$(PYTHON) buildsys/run_python_release.py \
		--static-library "$(PYTHON_KERNEL_STATIC_LIBRARY)" \
		--link-options "$(PYTHON_KERNEL_STATIC_LINK_OPTIONS)" \
		--static-receipt "$(PYTHON_KERNEL_STATIC_RECEIPT)" \
		--resource-root "$(PYTHON_KERNEL_RESOURCE_ROOT)" \
		--output-dir "$(PYTHON_RELEASE_WHEEL_ROOT)" \
		--auditwheel "$(AUDITWHEEL)" --readelf "$(READELF)"

$(PYTHON_RELEASE_CONTRACT_EVIDENCE): $(PYTHON_RELEASE_WORKFLOW) \
		$(PYPI_RELEASE_WORKFLOW) \
		$(PYTHON_RELEASE_BASELINE) $(PYTHON_RELEASE_TOOLCHAIN_LOCK) VERSION \
		buildsys/check_python_release_workflow.py buildsys/workflow_contract.py \
		buildsys/requirements.txt \
		buildsys/check_python_release.py buildsys/repair_python_wheel.py \
		buildsys/run_python_release.py \
		buildsys/python_wheel_receipt.py | doctor-workflow
	@$(PYTHON) buildsys/check_python_release_workflow.py \
		--workflow "$(PYTHON_RELEASE_WORKFLOW)" \
		--pypi-workflow "$(PYPI_RELEASE_WORKFLOW)" \
		--baseline "$(PYTHON_RELEASE_BASELINE)" \
		--toolchain-lock "$(PYTHON_RELEASE_TOOLCHAIN_LOCK)" --output "$@"

python-release-contract-check: $(PYTHON_RELEASE_CONTRACT_EVIDENCE)

python-release-evidence-check: python-product-check python-release-contract-check
	@$(PYTHON) buildsys/check_portability.py --root "$(ROOT)" \
		--output "$(PYTHON_PORTABILITY_EVIDENCE)"
	@$(PYTHON) buildsys/check_python_release_candidate.py \
		--root "$(ROOT)" \
		--baseline "$(PYTHON_RELEASE_BASELINE)" \
		--embedded-release "$(EMBEDDED_RELEASE_EVIDENCE)" \
		--wheel-receipt "$(PYTHON_WHEEL_RECEIPT)" \
		--wheel-evidence "$(PYTHON_WHEEL_EVIDENCE)" \
		--workflow-contract "$(PYTHON_RELEASE_CONTRACT_EVIDENCE)" \
		--portability "$(PYTHON_PORTABILITY_EVIDENCE)" \
		--output "$(PYTHON_RELEASE_CANDIDATE_EVIDENCE)"

python-product-check: embedded-release-full-check python-check portability-check

python-release-candidate-check: python-release-evidence-check

$(RELEASE_CANDIDATE_RECEIPT): python-release-candidate-check static-sdk-package docs-check \
		$(PYTHON_RELEASE_CANDIDATE_EVIDENCE) $(EMBEDDED_CONFORMANCE_EVIDENCE) \
		$(EMBEDDED_KERNEL_STATIC_SDK_EVIDENCE) \
		$(STATIC_SDK_RELEASE_RECEIPT) $(STATIC_SDK_RELEASE_ARCHIVE) \
		$(STATIC_SDK_RELEASE_CHECKSUM) $(PYTHON_WHEEL_RECEIPT) \
		$(PYTHON_WHEEL_EVIDENCE) $(DOCS_EVIDENCE) \
		$(PUBLIC_SURFACE_EVIDENCE) $(PUBLIC_ARTIFACT_EVIDENCE) \
		buildsys/check_release_candidate.py
	@$(PYTHON) buildsys/check_release_candidate.py \
		--root "$(ROOT)" --version "$(POSTGAMMA_PRODUCT_VERSION)" \
		--python-release "$(PYTHON_RELEASE_CANDIDATE_EVIDENCE)" \
		--conformance "$(EMBEDDED_CONFORMANCE_EVIDENCE)" \
		--static-evidence "$(EMBEDDED_KERNEL_STATIC_SDK_EVIDENCE)" \
		--static-receipt "$(STATIC_SDK_RELEASE_RECEIPT)" \
		--static-archive "$(STATIC_SDK_RELEASE_ARCHIVE)" \
		--static-checksum "$(STATIC_SDK_RELEASE_CHECKSUM)" \
		--wheel-receipt "$(PYTHON_WHEEL_RECEIPT)" \
		--wheel-evidence "$(PYTHON_WHEEL_EVIDENCE)" \
		--wheel-directory "$(PYTHON_WHEEL_DIR)" \
		--docs-evidence "$(DOCS_EVIDENCE)" \
		--publication-evidence "$(PUBLIC_SURFACE_EVIDENCE)" \
		--artifact-publication-evidence "$(PUBLIC_ARTIFACT_EVIDENCE)" \
		--docs-site "$(DOCS_SITE_DIR)" --output "$@"

release-candidate: override PG_CONFIGURE_ARGS := $(strip \
	$(POSTGAMMA_FULL_TEST_BASE_PG_CONFIGURE_ARGS) \
	$(POSTGAMMA_FULL_TEST_REQUIRED_PG_CONFIGURE_ARGS))
release-candidate: doctor $(RELEASE_CANDIDATE_RECEIPT)

$(STATIC_SDK_RELEASE_ARCHIVE) $(STATIC_SDK_RELEASE_CHECKSUM) \
		$(STATIC_SDK_RELEASE_RECEIPT) &: \
		$(EMBEDDED_KERNEL_STATIC_SDK_EVIDENCE) \
		$(EMBEDDED_RELEASE_RESOURCE_EVIDENCE) \
		$(EMBEDDED_KERNEL_PUBLIC_API_STATIC_LIBRARY) \
		$(EMBEDDED_KERNEL_STATIC_LINK_OPTIONS) \
		$(EMBEDDED_KERNEL_STATIC_PKG_CONFIG) \
		$(EMBEDDED_KERNEL_STATIC_CORE_HEADER) \
		$(EMBEDDED_KERNEL_STATIC_ARROW_HEADER) \
		$(EMBEDDED_KERNEL_STATIC_EXTENSION_HEADER) \
		$(EMBEDDED_API_RELEASE_EXAMPLE) $(POSTGAMMA_POSTGRESQL_LICENSE) \
		$(POSTGAMMA_PGVECTOR_LICENSE) \
		$(POSTGAMMA_PROJECT_LICENSE) $(POSTGAMMA_PROJECT_NOTICE) \
		$(POSTGAMMA_THIRD_PARTY_NOTICES) VERSION \
		buildsys/package_static_sdk.py
	@$(PYTHON) buildsys/package_static_sdk.py \
		--sdk-root "$(EMBEDDED_KERNEL_BUILD)" \
		--resource-root "$(EMBEDDED_RELEASE_RESOURCE_ROOT)" \
		--example "$(EMBEDDED_API_RELEASE_EXAMPLE)" \
		--project-license "$(POSTGAMMA_PROJECT_LICENSE)" \
		--project-notice "$(POSTGAMMA_PROJECT_NOTICE)" \
		--third-party-notices "$(POSTGAMMA_THIRD_PARTY_NOTICES)" \
		--postgresql-license "$(POSTGAMMA_POSTGRESQL_LICENSE)" \
		--pgvector-license "$(POSTGAMMA_PGVECTOR_LICENSE)" \
		--version "$(POSTGAMMA_PRODUCT_VERSION)" \
		--platform "$(POSTGAMMA_STATIC_SDK_PLATFORM)" \
		--source-date-epoch "$(POSTGAMMA_STATIC_SDK_SOURCE_DATE_EPOCH)" \
		--output "$(STATIC_SDK_RELEASE_ARCHIVE)" \
		--checksum "$(STATIC_SDK_RELEASE_CHECKSUM)" \
		--receipt "$(STATIC_SDK_RELEASE_RECEIPT)"

static-sdk-package: doctor-sdk version-check $(STATIC_SDK_RELEASE_ARCHIVE) \
		$(STATIC_SDK_RELEASE_CHECKSUM) \
		$(STATIC_SDK_RELEASE_RECEIPT)

$(PUBLIC_ARTIFACT_EVIDENCE): $(STATIC_SDK_RELEASE_ARCHIVE) \
		$(PYTHON_WHEEL_RECEIPT) buildsys/check_public_artifacts.py
	@$(PYTHON) buildsys/check_public_artifacts.py --root "$(ROOT)" \
		--static-archive "$(STATIC_SDK_RELEASE_ARCHIVE)" \
		--wheel-receipt "$(PYTHON_WHEEL_RECEIPT)" \
		--wheel-directory "$(PYTHON_WHEEL_DIR)" --output "$@"

artifact-publication-check: $(PUBLIC_ARTIFACT_EVIDENCE)

# Invalidate packaged bytes when their orchestration changes.
$(PYTHON_WHEEL_RECEIPT) $(STATIC_SDK_RELEASE_ARCHIVE) \
		$(STATIC_SDK_RELEASE_CHECKSUM) $(STATIC_SDK_RELEASE_RECEIPT): buildsys/make/release.mk
