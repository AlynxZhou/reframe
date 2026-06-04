#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>

/* Include the converter header - adjust path as needed */
#include "reframe-server/rf-converter.h"

static sigjmp_buf jump_buffer;
static volatile sig_atomic_t got_signal = 0;

static void signal_handler(int sig) {
    got_signal = 1;
    siglongjmp(jump_buffer, 1);
}

START_TEST(test_buffer_bounds_on_oversized_input)
{
    /* Invariant: Buffer reads never exceed the declared length */
    
    /* Create oversized payloads: 2x and 10x typical buffer sizes */
    size_t sizes[] = { 64, 512, 5120 };  /* normal, 2x, 10x */
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);
    
    struct sigaction sa, old_sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, &old_sa);
    sigaction(SIGBUS, &sa, &old_sa);
    
    for (int i = 0; i < num_sizes; i++) {
        got_signal = 0;
        char *payload = malloc(sizes[i] + 1);
        ck_assert_ptr_nonnull(payload);
        memset(payload, 'A', sizes[i]);
        payload[sizes[i]] = '\0';
        
        if (sigsetjmp(jump_buffer, 1) == 0) {
            /* Call the actual converter function with oversized data */
            rf_converter_t *conv = rf_converter_create();
            if (conv) {
                rf_converter_process(conv, (uint8_t *)payload, sizes[i]);
                rf_converter_destroy(conv);
            }
        }
        
        /* Assert no crash occurred from out-of-bounds access */
        ck_assert_msg(got_signal == 0, 
            "Out-of-bounds access detected with payload size %zu", sizes[i]);
        
        free(payload);
    }
    
    sigaction(SIGSEGV, &old_sa, NULL);
    sigaction(SIGBUS, &old_sa, NULL);
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_bounds_on_oversized_input);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}