;;; status-compare.el --- Magit buffers with and without jl-git-helper -*- lexical-binding: t; -*-

;; emacs --batch -Q -l status-compare.el REPO HELPER ELPA-DIR LISP-DIR...
;;
;; Renders the status buffer and a log buffer of REPO three times: git
;; only, with the helper, and in verify mode.  Fails unless the text is
;; identical every time and verify mode saw no difference.  Prints the
;; trace of the helper run.

(pcase-let ((`(,repo ,helper ,elpa . ,lisp) command-line-args-left))
  (setq command-line-args-left nil)
  (setq package-user-dir elpa)
  (package-initialize)
  (dolist (d lisp) (add-to-list 'load-path d))
  (require 'magit)
  (require 'jl-magit-prefill)
  (require 'jl-magit-trace)
  (setq magit-refresh-verbose nil
        magit-refresh-status-buffer nil)
  ;; The user's status sections.
  (remove-hook 'magit-status-headers-hook 'magit-insert-tags-header)
  (remove-hook 'magit-status-sections-hook 'magit-insert-unpushed-to-pushremote)
  (remove-hook 'magit-status-sections-hook 'magit-insert-unpulled-from-pushremote)
  (require 'jl-magit-git-helper)
  (setq jl/magit-git-helper-program helper)

  (let ((default-directory (file-name-as-directory (expand-file-name repo)))
        results failed)
    (cl-flet ((render (mode)
                (setq jl/magit-git-helper-mode mode)
                (let* ((status (progn (magit-status-setup-buffer default-directory)
                                      (current-buffer)))
                       (log (save-window-excursion
                              (magit-log-setup-buffer (list "HEAD") '("-n256" "--decorate") nil)
                              (current-buffer))))
                  (with-current-buffer status
                    (if (eq mode t)
                        (progn (jl/magit-trace-refresh)
                               (princ (with-current-buffer "*jl-magit-trace*"
                                        (buffer-string))))
                      (magit-refresh-buffer)))
                  (with-current-buffer log (magit-refresh-buffer))
                  (list (with-current-buffer status
                          (buffer-substring-no-properties (point-min) (point-max)))
                        (with-current-buffer log
                          (buffer-substring-no-properties (point-min) (point-max)))))))
      (let ((plain (render nil))
            (helped (render t))
            (verified (render 'verify)))
        (princ (format "status buffer: %d lines\n%s\n"
                       (length (split-string (car plain) "\n"))
                       (car plain)))
        (dolist (pair `(("helper" . ,helped) ("verify" . ,verified)))
          (dolist (i '(0 1))
            (unless (equal (nth i plain) (nth i (cdr pair)))
              (setq failed t)
              (princ (format "DIFFERENT %s buffer with %s:\n--- git only\n%s\n--- %s\n%s\n"
                             (if (= i 0) "status" "log") (car pair)
                             (nth i plain) (car pair) (nth i (cdr pair)))))))
        (let ((mism (plist-get jl/magit-git-helper-stats :mismatches)))
          (princ (format "verify mode: %d compared, %d mismatches\n"
                         (plist-get jl/magit-git-helper-stats :verified) mism))
          (when (> mism 0)
            (setq failed t)
            (princ (with-current-buffer "*jl-git-helper log*" (buffer-string)))))
        (princ (if failed "FAILED\n" "OK: identical buffers\n"))
        (kill-emacs (if failed 1 0))))))
