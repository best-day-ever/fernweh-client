# Read deployment settings in the qmake process environment on every platform.
FERNWEH_CONFIG_PYTHON = $$(FERNWEH_CONFIG_PYTHON)
isEmpty(FERNWEH_CONFIG_PYTHON) {
    win32: FERNWEH_CONFIG_PYTHON = python
    else: FERNWEH_CONFIG_PYTHON = python3
}
FERNWEH_BROKER_ENABLED = $$(FERNWEH_BROKER_ENABLED)
isEmpty(FERNWEH_BROKER_ENABLED): FERNWEH_BROKER_ENABLED = 0
FERNWEH_QML_RESOURCE = $$OUT_PWD/fernweh-resources/qml.qrc
fernweh_config_command = $$shell_quote($$FERNWEH_CONFIG_PYTHON) $$shell_quote($$PWD/configure.py) --output $$shell_quote($$OUT_PWD/fernwehdeploymentconfig.h) --qml-output $$shell_quote($$OUT_PWD/fernweh-resources)
fernweh_config_output = $$system($$fernweh_config_command, lines, fernweh_config_status)
!equals(fernweh_config_status, 0): error("Fernweh deployment configuration failed; see the validation error above")
INCLUDEPATH += $$OUT_PWD
DEFINES += FERNWEH_DEPLOYMENT_CONFIGURED
# Editing either source view regenerates its selected build resource.
QMAKE_INTERNAL_INCLUDED_FILES += $$PWD/configure.py $$PWD/../app/qml.qrc $$PWD/../app/gui/main.qml $$PWD/../app/gui/SettingsView.qml
QMAKE_INTERNAL_INCLUDED_FILES += $$PWD/../app/gui/DisplaySetupDialog.qml $$PWD/../app/gui/DisplayAdvancedPage.qml $$PWD/../app/gui/DisplayCompatibilityPage.qml
