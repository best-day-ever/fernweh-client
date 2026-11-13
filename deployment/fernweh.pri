# Read deployment settings in the qmake process environment on every platform.
FERNWEH_CONFIG_PYTHON = $$(FERNWEH_CONFIG_PYTHON)
isEmpty(FERNWEH_CONFIG_PYTHON) {
    win32: FERNWEH_CONFIG_PYTHON = python
    else: FERNWEH_CONFIG_PYTHON = python3
}
fernweh_config_command = $$shell_quote($$FERNWEH_CONFIG_PYTHON) $$shell_quote($$PWD/configure.py) --output $$shell_quote($$OUT_PWD/fernwehdeploymentconfig.h)
fernweh_config_output = $$system($$fernweh_config_command, lines, fernweh_config_status)
!equals(fernweh_config_status, 0): error("Fernweh deployment configuration failed; see the validation error above")
INCLUDEPATH += $$OUT_PWD
DEFINES += FERNWEH_DEPLOYMENT_CONFIGURED
