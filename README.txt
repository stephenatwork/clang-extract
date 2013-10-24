
UPDATE Oct-2013:
=============

Originally we had chose python as the main language for this project because it was a natural fit
for text processing. However, we eventually reached a point where the simplified output wasn't
powerful enough to handle all the nuances of templates and specialization. Since then, we have
ported our scripts to C++ so that they can access the Clang AST directly.

The C++ version is noticably faster and allows full clang AST access. This python version allows
rapid iteration and is more suitable for experimentation.



Clang-extract
=============


Clang-extract uses clang to parse your header files and then prints out a description of what it parsed.

This project was described in a talk a GDC 2012 http://www.gdcvault.com/play/1015586/ (subscription required)
It was funded by Havok and is used as the basis of its reflection system.


Building
--------
* Either in your environment or in the Makefile, set LLVM_DIR to the folder containing a prebuilt LLVM and Clang.
* make


Test
--------
To run clang-extract on a small test:
* make test

You'll notice the output format is actually python code so you can parse it thus :
def parse(text):
	output = # your output structure
	def Method(id, recordid, typeid, name, static):
		# code here for a method, modifids output
	# more local function definitions here
    exec text in locals()
	return output


Invoking
--------
Run clang-extract --help to see command line options.


Notes
--------
clang-extract internally creates a file which includes all the input files specified on the command line.
You may need to add "-I ." to find the input files.
The -A option is useful to pass through annotations which are stored in the output file.
