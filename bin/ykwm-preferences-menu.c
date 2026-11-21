
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yk/bundle.h>
#include <yk/entry.h>

#define MENU_NAME "YKPreferencesMenu"
#define METADATA_FILE "app.metadata"
#define PREFERENCES_SECTION "Preferences add Entry"

/*
 * Print a string as a quoted FVWM menu argument.
 * Escape quotes and backslashes inside the string.
 */
static void print_menu_string(const char *str) {


    for (const char *p = str; *p; p++) {
        

        putchar(*p);
    }
}

/*
 * Process one application's Preferences entries.
 */
static void process_app(const ykApp *app) {
    char metadata_path[PATH_MAX_LEN];

    int len = snprintf(
        metadata_path,
        sizeof(metadata_path),
        "%s/%s",
        app->bundle_path,
        METADATA_FILE
    );

    if (len < 0 || (size_t)len >= sizeof(metadata_path))
        return;

    EntryDocument *doc = parse_entry_file(metadata_path);

    if (!doc)
        return;

    int name_count = 0;
    int argument_count = 0;

    const char *const *names = get_entry_values(
        doc,
        PREFERENCES_SECTION,
        "Name",
        &name_count
    );

    const char *const *arguments = get_entry_values(
        doc,
        PREFERENCES_SECTION,
        "Argument",
        &argument_count
    );

    if (names && arguments) {
        int count = name_count < argument_count
                  ? name_count
                  : argument_count;

        for (int i = 0; i < count; i++) {
            if (!names[i] || !*names[i])
                continue;

            if (!arguments[i] || !*arguments[i])
                continue;

            printf("AddToMenu %s ", MENU_NAME);

            print_menu_string(names[i]);

            printf(" Exec exec yk-runner \"");

            print_menu_string(app->name);


            printf("\" --%s\n", arguments[i]);
        }
    }

    free_entry_document(doc);
}

int main(void) {
    ykApp apps[YK_MAX_APPS];
    int app_count = 0;

    if (yk_list_apps("YK_APPS_PATH", apps, &app_count) != 0) {
        fprintf(stderr, "Failed to list YeKara applications\n");
        return 1;
    }

    printf("DestroyMenu %s\n", MENU_NAME);

    for (int i = 0; i < app_count; i++)
        process_app(&apps[i]);

    return 0;
}
