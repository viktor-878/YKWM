default_confdir = join_paths(
    get_option('datadir'),
    meson.project_name(),
    'default-config',
)

default_conf_files = [
    'components/toolbox.fvwm',
    'components/images/defaultIcon.xpm',
    'components/core.fvwm',
    'components/coreBindings.fvwm',
    'components/ROOTMENU.fvwm',
    'components/transientConfig.fvwm',
    'colors',
    'startup.wav',
    'config',
]

install_data(
    default_conf_files,
    install_dir: default_confdir,
    preserve_path: true,
)
