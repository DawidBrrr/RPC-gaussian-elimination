struct Matrix{
    unsigned int rows;
    unsigned int cols;
    double data<>;
};


struct Solution{
    double values<>;
};

program GAUSS_RPC{
    version GAUSS_V{
        Solution SOLVE_GAUSS(Matrix) = 1;
    } = 1;
} = 0x20000001;