(load "compiler-test-util.lisp")

(with-test (:name :block-inline-then-notinline)
  (ctu:file-compile
   `((in-package :cl-user)
     (declaim (inline block-inline-foo))
     (defun block-inline-foo (a b)
       (+ a b))
     (defun bar-with-foo-inline (a b c)
       (* c (block-inline-foo a b)))

     (declaim (notinline block-inline-foo))
     (defun bar-with-foo-call (a b c)
       (* c (block-inline-foo a b))))
   :block-compile t
   :load t)
  (assert (not (ctu:find-named-callees (symbol-function 'bar-with-foo-inline))))
  (assert (ctu:find-named-callees (symbol-function 'bar-with-foo-call))))

(with-test (:name :block-defpackage-then-load-fasl)
  (ctu:file-compile
   `((defpackage block-defpackage (:use :cl :cl-user))

     (in-package :block-defpackage)

     (defstruct (struct-foo (:conc-name "FOO-"))
       (bar 0 :type number)
       (baz nil :type list)))
   :block-compile t
   :before-load (lambda () (delete-package :block-defpackage))
   :load t))

(defvar *x*)
(defvar *y*)

(with-test (:name :block-defpackage-top-level-form-order)
  (ctu:file-compile
   `((setq *x* (find-package "BLOCK-DEFPACKAGE"))

     (defpackage block-defpackage (:use :cl :cl-user))

     (setq *y* (find-package "BLOCK-DEFPACKAGE")))
   :block-compile t
   :before-load (lambda () (delete-package :block-defpackage))
   :load t)
  (assert (eq *x* nil))
  (assert *y*))

(with-test (:name :block-defconstant-then-load-fasl)
  (ctu:file-compile
   ;; test a non-EQL-comparable constant, so that uses of it need symbol-value
   ;; at load-time.
   `((defconstant testconstant '(1 2 3 4 5))
     (defun foo ()
       (loop for i in testconstant collect i)))
   :block-compile t
   :before-load (lambda () (unintern (find-symbol "TESTCONSTANT")))
   :load t))

(with-test (:name :block-defconstant-hairy-then-load-fasl)
  (ctu:file-compile
   `((sb-int:defconstant-eqx testconstant2
         (let (list)
           (dotimes (i 5 list) (push i list)))
       #'equal)
     (defun bar ()
       (loop for i in testconstant2 collect i)))
   :block-compile t
   :before-load (lambda () (unintern (find-symbol "TESTCONSTANT2")))
   :load t))