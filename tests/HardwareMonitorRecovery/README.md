# HardwareMonitor recovery validation

This harness belongs to the fork's validation branch. The upstream repair contains
only the plugin source changes; this workflow and instrumentation are not part of
the upstream pull request.

The Windows job first builds each version's unmodified production project with
MSBuild, v143 C++/CLI and the bundled LibreHardwareMonitor assembly. It then builds
a separate regression project from the same source and WinForms resources. Only
`Computer` acquisition and modal diagnostics are substituted in that project.
The substitute uses the real LHM interfaces and hardware settings properties,
but never calls `Computer.Open`, installs drivers, or changes device state.

The regression follows the host callbacks: `EI_CONFIG_DIR`, `OnInitialize`,
`GetInfo`, `GetItem` enumeration, then `DataRequired`. It checks:

- a startup `Open` failure still loads two saved choices, including an unavailable
  sensor, all hardware flags, formatting/notification settings and tree state;
- settings and hardware dialogs can be constructed while the provider is absent;
- saving both offline and after retries preserves the INI contents;
- retry uses the saved enable flags and preserves the item pointers and IDs that
  the host enumerated;
- a sensor appearing after a previously unsuccessful lookup resolves without
  another rebuild, and its metadata/value become available;
- a later update failure, a failed rebuild and a successful retry replace the
  old sensor references without losing user choices;
- an available sensor above a notification threshold does not trigger a value
  notification during configuration loading, before `OnInitialize`; the first
  normal `DataRequired` call emits the notification afterward.

The live-startup control must pass for both versions. The original commit
`9bf27fb0d05e2ef88c6ee16f865f88601cf44885` must fail specifically at the assertion
that startup failure discarded the configured choices. An arbitrary build error,
crash, timeout or different assertion is not accepted as a reproduced regression.
The repaired version must pass both scenarios. Logs and production builds are
uploaded as CI evidence.

This validates Windows compilation and plugin recovery logic with controlled
provider failures. It does not claim to reproduce an actual NVIDIA driver update
or verify physical hardware monitoring. Such a claim needs a separate, explicitly
authorized hardware test.
